// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "irc_staged.h"

#include <array>
#include <bit>
#include <limits>
#include <stdexcept>

namespace ekphore::irc::native {

ForwardPrefix ForwardPrefix::prepare(std::uint32_t block,
        std::span<const std::int32_t> indices,
        std::span<const std::uint8_t> flags,
        std::span<const std::int32_t> auxiliary, std::size_t end,
        std::size_t twiddle_extent) {
    if (block < 64 || block > (1U << 29) || !std::has_single_bit(block) ||
            end < block / 4 || end > indices.size() || end > flags.size() ||
            twiddle_extent >
                    std::size_t(std::numeric_limits<std::int32_t>::max()))
        throw std::invalid_argument(
                "invalid ordinary FFT forward-prefix dimensions");
    ForwardPrefix result;
    result.extent = block * 2;
    result.root_extent = twiddle_extent;
    result.instruction_records.reserve(end);
    std::uint32_t stride = block, shift = 0;
    std::size_t auxiliary_cursor = 0;
    const auto packet = [&](std::uint64_t index) {
        if (index + 4 > result.extent)
            throw std::invalid_argument("FFT prefix packet outside plane");
        return static_cast<std::uint32_t>(index);
    };
    for (std::size_t i = 0; i < end; ++i) {
        if (indices[i] < 0)
            throw std::invalid_argument("negative FFT prefix index");
        const auto first = packet(static_cast<std::uint32_t>(indices[i]));
        PrefixInstruction instruction{ PrefixOperation::real_pair, first, 0, 0,
            0 };
        if (i < block / 4) {
            if (first % 4)
                result.ring_prefix_compatible = false;
            // only the real plane, with fixed separation B
            instruction.second = packet(std::uint64_t(first) + block);
        } else if (!flags[i]) {
            instruction.operation = PrefixOperation::complex_pair;
            instruction.second = packet(std::uint64_t(first) + stride);
        } else {
            if (auxiliary_cursor >= auxiliary.size() ||
                    auxiliary[auxiliary_cursor] < 0)
                throw std::invalid_argument("missing FFT prefix twiddle index");
            const auto base = std::uint64_t(auxiliary[auxiliary_cursor++])
                              << shift;
            const auto step = std::uint64_t(1) << shift;
            const auto last = base + 3 * step;
            const bool second_half =
                    first > stride; // equality stays in the first half
            const auto last_twiddle = second_half ? 3 * last : last;
            if (last_twiddle >= twiddle_extent)
                throw std::invalid_argument("FFT prefix twiddle outside table");
            instruction.operation = second_half
                                            ? PrefixOperation::twiddle_pair
                                            : PrefixOperation::twiddle_first;
            instruction.second = packet(std::uint64_t(first) + stride / 2);
            instruction.twiddle_base = static_cast<std::uint32_t>(base);
            instruction.twiddle_step = static_cast<std::uint32_t>(step);
        }
        result.instruction_records.push_back(instruction);
        // the first one after the head does NOT advance even if zero, later
        // zeros advance once
        if (i >= block / 4 && i + 1 < end && indices[i + 1] == 0) {
            if (++shift >= 31 || !(stride /= 2))
                throw std::invalid_argument(
                        "FFT prefix stage outside qualified domain");
        }
    }
    return result;
}

bool ForwardPrefix::execute_ring(std::span<const float> ring, std::size_t head,
        std::span<float> real, std::size_t first,
        std::size_t count) const noexcept {
    const auto records = extent / 8;
    if (!ring_prefix_compatible || ring.size() < extent ||
            ring.size() >
                    std::size_t(std::numeric_limits<std::int32_t>::max()) ||
            !std::has_single_bit(ring.size()) || head >= ring.size() ||
            head % 4 || real.size() < extent || first > records ||
            count > records - first)
        return false;
    for (auto op : std::span(instruction_records).subspan(first, count)) {
        const auto a = (head + op.first) & (ring.size() - 1);
        const auto b = (a + extent / 2) & (ring.size() - 1);
        std::array<float, 4> x, y;
        for (unsigned lane = 0; lane < 4; ++lane) {
            x[lane] = ring[a + lane];
            y[lane] = ring[b + lane];
        }
        for (unsigned lane = 0; lane < 4; ++lane)
            real[op.second + lane] = x[lane] - y[lane];
        for (unsigned lane = 0; lane < 4; ++lane)
            real[op.first + lane] = x[lane] + y[lane];
    }
    return true;
}

bool ForwardPrefix::execute(std::span<float> real, std::span<float> imaginary,
        std::span<const float> cosine, std::span<const float> sine,
        std::size_t first_record, std::size_t count) const noexcept {
    if (real.size() < extent || imaginary.size() < extent ||
            cosine.size() < root_extent || sine.size() < root_extent ||
            first_record > instruction_records.size() ||
            count > instruction_records.size() - first_record)
        return false;
    for (const auto &operation :
            std::span(instruction_records).subspan(first_record, count)) {
        const auto first = operation.first, second = operation.second;
        std::array<float, 4> real_a, real_b, imag_a{}, imag_b{};
        // read all of it BEFORE writing
        for (std::size_t lane = 0; lane < 4; ++lane) {
            real_a[lane] = real[first + lane];
            real_b[lane] = real[second + lane];
            if (operation.operation != PrefixOperation::real_pair) {
                imag_a[lane] = imaginary[first + lane];
                imag_b[lane] = imaginary[second + lane];
            }
        }
        if (operation.operation == PrefixOperation::real_pair ||
                operation.operation == PrefixOperation::complex_pair) {
            // difference BEFORE sum
            for (std::size_t lane = 0; lane < 4; ++lane) {
                real[second + lane] = real_a[lane] - real_b[lane];
                if (operation.operation == PrefixOperation::complex_pair)
                    imaginary[second + lane] = imag_a[lane] - imag_b[lane];
            }
            for (std::size_t lane = 0; lane < 4; ++lane) {
                real[first + lane] = real_a[lane] + real_b[lane];
                if (operation.operation == PrefixOperation::complex_pair)
                    imaginary[first + lane] = imag_a[lane] + imag_b[lane];
            }
        } else {
            // NOT fused
            for (std::size_t lane = 0; lane < 4; ++lane) {
                const auto k =
                        operation.twiddle_base + lane * operation.twiddle_step;
                const auto sum = real_a[lane] + imag_b[lane];
                const auto difference = imag_a[lane] - real_b[lane];
                real[first + lane] = sum * cosine[k] - difference * sine[k];
                imaginary[first + lane] =
                        difference * cosine[k] + sum * sine[k];
            }
            if (operation.operation == PrefixOperation::twiddle_pair) {
                for (std::size_t lane = 0; lane < 4; ++lane) {
                    const auto k = 3 * (operation.twiddle_base +
                                               lane * operation.twiddle_step);
                    const auto difference = real_a[lane] - imag_b[lane];
                    const auto sum = imag_a[lane] + real_b[lane];
                    real[second + lane] =
                            difference * cosine[k] - sum * sine[k];
                    imaginary[second + lane] =
                            sum * cosine[k] + difference * sine[k];
                }
            }
        }
    }
    return true;
}

} // namespace ekphore::irc::native

#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace ekphore::irc::native {

ForwardTerminal ForwardTerminal::prepare(std::uint32_t block,
        std::span<const std::int32_t> indices,
        std::span<const std::uint8_t> flags,
        std::span<const std::uint32_t> permutation, std::size_t prefix_end,
        std::size_t twiddle_extent) {
    constexpr auto maximum =
            std::size_t(std::numeric_limits<std::int32_t>::max());
    if (block < 64 || block > (1U << 29) || !std::has_single_bit(block) ||
            indices.size() > maximum || indices.size() < block / 2 + 1 ||
            flags.size() < indices.size() || prefix_end < block / 4 ||
            prefix_end > indices.size() || permutation.size() < block * 2 ||
            twiddle_extent > maximum)
        throw std::invalid_argument("invalid ordinary FFT terminal dimensions");
    ForwardTerminal result;
    result.extent = block * 2;
    result.twiddle_one = result.extent / 8;
    result.twiddle_three = result.twiddle_one * 3;
    result.root_extent = twiddle_extent;
    const auto end = indices.size() - block / 2 - 1;
    result.instruction_records.reserve(
            end > prefix_end ? (end - prefix_end + 1) / 2 : 0);
    const auto mirror = [&](std::uint32_t source) {
        const auto reversed = permutation[source];
        if (!reversed || reversed >= result.extent)
            throw std::invalid_argument(
                    "terminal mirrored index outside permutation");
        const auto destination = permutation[result.extent - reversed];
        if (destination >= result.extent)
            throw std::invalid_argument("terminal destination outside plane");
        return destination;
    };
    auto cursor = prefix_end;
    for (; cursor < end; cursor += raw_entries_per_instruction) {
        if (indices[cursor] < 0 ||
                std::uint64_t(indices[cursor]) + 4 > result.extent)
            throw std::invalid_argument("terminal source packet outside plane");
        const auto first = static_cast<std::uint32_t>(indices[cursor]);
        const bool rotated = flags[cursor] != 0;
        if (rotated && result.twiddle_three >= twiddle_extent)
            throw std::invalid_argument("terminal twiddle outside table");
        result.instruction_records.push_back(
                { first, mirror(first + 1), mirror(first + 3), rotated });
    }
    result.final_raw_cursor = cursor;
    return result;
}

bool ForwardTerminal::execute(std::span<float> real, std::span<float> imaginary,
        std::span<const float> cosine, std::span<const float> sine,
        std::size_t begin, std::size_t count) const noexcept {
    if (real.size() < extent || imaginary.size() < extent ||
            cosine.size() < root_extent || sine.size() < root_extent ||
            begin > instruction_records.size() ||
            count > instruction_records.size() - begin)
        return false;
    for (const auto &operation :
            std::span(instruction_records).subspan(begin, count)) {
        const auto first = operation.first;
        const std::array<float, 4> r{ real[first], real[first + 1],
            real[first + 2], real[first + 3] };
        const std::array<float, 4> i{ imaginary[first], imaginary[first + 1],
            imaginary[first + 2], imaginary[first + 3] };
        if (!operation.rotated) {
            // keep the parentheses and the store order, these can overlap
            real[operation.mirror_one] = (r[0] + r[2]) - (r[1] + r[3]);
            imaginary[operation.mirror_one] = -((i[0] + i[2]) - (i[1] + i[3]));
            real[first] = ((r[0] + r[1]) + r[2]) + r[3];
            imaginary[first] = ((i[0] + i[1]) + i[2]) + i[3];
            const auto even_real = r[0] - r[2], even_imaginary = i[0] - i[2];
            const auto odd_real = r[1] - r[3], odd_imaginary = i[1] - i[3];
            real[operation.mirror_three] = even_real - odd_imaginary;
            imaginary[operation.mirror_three] = -(odd_real + even_imaginary);
            real[first + 2] = even_real + odd_imaginary;
            imaginary[first + 2] = even_imaginary - odd_real;
        } else {
            const auto cross_sum = r[1] + i[3], cross_difference = r[1] - i[3];
            const auto cross_imaginary_difference = i[1] - r[3],
                       cross_imaginary_sum = r[3] + i[1];
            const auto c1 = cosine[twiddle_one], s1 = sine[twiddle_one];
            const auto c3 = cosine[twiddle_three], s3 = sine[twiddle_three];
            // multiply, then exactly one fma, NOT fnmadd
            const auto rotated_real_one =
                    std::fma(cross_sum, c1, s1 * -cross_imaginary_difference);
            const auto rotated_imaginary_one =
                    std::fma(cross_sum, s1, cross_imaginary_difference * c1);
            const auto rotated_real_three =
                    std::fma(cross_difference, c3, s3 * -cross_imaginary_sum);
            const auto rotated_imaginary_three =
                    std::fma(cross_difference, s3, cross_imaginary_sum * c3);
            const auto sum = r[0] + i[2], difference = i[0] - r[2];
            real[first] = sum + rotated_real_one;
            imaginary[first] = difference + rotated_imaginary_one;
            real[operation.mirror_one] = sum - rotated_real_one;
            imaginary[operation.mirror_one] =
                    -(difference - rotated_imaginary_one);
            const auto other_real = r[0] - i[2], other_imaginary = r[2] + i[0];
            real[first + 2] = other_real + rotated_real_three;
            imaginary[first + 2] = other_imaginary + rotated_imaginary_three;
            real[operation.mirror_three] = other_real - rotated_real_three;
            imaginary[operation.mirror_three] =
                    -(other_imaginary - rotated_imaginary_three);
        }
    }
    return true;
}

bool ForwardTerminal::pack(
        std::span<float> real, std::span<float> imaginary) const noexcept {
    if (real.size() < extent || imaginary.size() < extent)
        return false;
    imaginary[0] = real[1];
    const auto gain = 1.F / static_cast<float>(extent);
    for (std::uint32_t source = 2; source < extent; source += 2) {
        real[source / 2] = gain * real[source];
        imaginary[source / 2] = gain * imaginary[source];
    }
    real[0] = gain * real[0];
    imaginary[0] = gain * imaginary[0];
    return true;
}

bool ForwardTerminal::execute_runtime(std::span<const float> real,
        std::span<const float> imaginary, std::span<const float> cosine,
        std::span<const float> sine, std::span<float> output_real,
        std::span<float> output_imaginary, std::size_t begin,
        std::size_t count) const noexcept {
    if (real.size() < extent || imaginary.size() < extent ||
            cosine.size() < root_extent || sine.size() < root_extent ||
            output_real.size() < extent / 2 ||
            output_imaginary.size() < extent / 2 ||
            begin > instruction_records.size() ||
            count > instruction_records.size() - begin)
        return false;
    for (auto op : std::span(instruction_records).subspan(begin, count)) {
        const auto p = op.first;
        const std::array<float, 4> r{ real[p], real[p + 1], real[p + 2],
            real[p + 3] };
        const std::array<float, 4> i{ imaginary[p], imaginary[p + 1],
            imaginary[p + 2], imaginary[p + 3] };
        const auto a = p / 2, b = op.mirror_one / 2, c = (p + 2) / 2,
                   d = op.mirror_three / 2;
        if (!op.rotated) {
            output_real[b] = (r[0] + r[2]) - (r[1] + r[3]);
            output_imaginary[b] = -((i[0] + i[2]) - (i[1] + i[3]));
            output_real[a] = ((r[0] + r[1]) + r[2]) + r[3];
            output_imaginary[a] = ((i[0] + i[1]) + i[2]) + i[3];
            const auto er = r[0] - r[2], ei = i[0] - i[2];
            const auto odd_real = r[1] - r[3], odd_imag = i[1] - i[3];
            output_real[d] = er - odd_imag;
            output_imaginary[d] = -(odd_real + ei);
            output_real[c] = er + odd_imag;
            output_imaginary[c] = ei - odd_real;
        } else {
            const auto cross_sum = r[1] + i[3], cross_difference = r[1] - i[3];
            const auto imag_difference = i[1] - r[3], imag_sum = r[3] + i[1];
            const auto c1 = cosine[twiddle_one], s1 = sine[twiddle_one];
            const auto c3 = cosine[twiddle_three], s3 = sine[twiddle_three];
            const auto rr1 = std::fma(cross_sum, c1, s1 * (-imag_difference));
            const auto ri1 = std::fma(cross_sum, s1, imag_difference * c1);
            const auto rr3 = std::fma(cross_difference, c3, s3 * (-imag_sum));
            const auto ri3 = std::fma(cross_difference, s3, imag_sum * c3);
            const auto sum = r[0] + i[2], difference = i[0] - r[2];
            output_real[a] = sum + rr1;
            output_imaginary[a] = difference + ri1;
            output_real[b] = sum - rr1;
            output_imaginary[b] = -(difference - ri1);
            const auto other_real = r[0] - i[2], other_imag = r[2] + i[0];
            output_real[c] = other_real + rr3;
            output_imaginary[c] = other_imag + ri3;
            output_real[d] = other_real - rr3;
            output_imaginary[d] = -(other_imag - ri3);
        }
    }
    return true;
}
bool ForwardTerminal::finish_runtime(std::span<const float> real,
        std::span<float> imaginary) const noexcept {
    if (real.size() < extent || imaginary.size() < extent / 2)
        return false;
    imaginary[0] = ((real[0] + real[2]) - real[1]) - real[3];
    return true;
}

} // namespace ekphore::irc::native

#include <limits>

namespace ekphore::irc::native {
static bool run_forward_stages_impl(const MonoWorkspace::Tier &plan,
        MonoTierRuntime &runtime, std::span<const float> ring,
        ForwardStageAdmissions *admissions, StereoRightRuntime *right,
        std::span<const float> right_ring) noexcept {
    const auto &c = runtime.config;
    auto &s = runtime.state;
    auto &cycle = s.cycle;
    const auto b = c.block, quarter = b / 4;
    // clang-format off
    if (!plan.forward || !plan.terminal || b < 64 || b > (1U << 21) ||
            !std::has_single_bit(b) || ring.size() < 2 * b ||
            !std::has_single_bit(ring.size()) || cycle.input_head >=
            ring.size() || cycle.input_head % 4 || ring.size() >
            std::size_t(std::numeric_limits<std::int32_t>::max()) ||
            s.completed_frames > 64 || cycle.progress >
            c.available_forward_frames || cycle.work_cursor >
            plan.terminal->raw_cursor_after() ||
            !c.quotas.forward_entries_per_frame ||
            !c.quotas.first_forward_frames ||
            !c.quotas.remaining_forward_frames || plan.work.forward_prefix_end <
            quarter || plan.forward->instructions().size() !=
            plan.work.forward_prefix_end || runtime.working[0].size() < 2 * b ||
            runtime.working[1].size() < 2 * b ||
            std::uint64_t(cycle.history_offset) + b >
            runtime.history[0].size() || std::uint64_t(cycle.history_offset) +
            b > runtime.history[1].size() || (cycle.progress <
            c.quotas.first_forward_frames && (!s.first_remaining ||
            cycle.work_cursor > quarter)))
        // clang-format on
        return false;
    // clang-format off
    if (right && (right_ring.size() != ring.size() || right->working[0].size() <
            2 * b || right->working[1].size() < 2 * b ||
            std::uint64_t(cycle.history_offset) + b >
            right->history[0].size() || std::uint64_t(cycle.history_offset) +
            b > right->history[1].size()))
        // clang-format on
        return false;
    if (admissions)
        *admissions = {};
    auto real = runtime.working[0], imaginary = runtime.working[1];
    auto packed_real = runtime.history[0].subspan(cycle.history_offset, b);
    auto packed_imaginary = runtime.history[1].subspan(cycle.history_offset, b);
    const auto advance = [&](std::uint32_t frames) {
        s.input_remaining -= frames;
        cycle.progress += frames;
        s.forward_remaining -= frames;
        s.completed_frames += frames;
    };
    const auto middle_end = c.quotas.first_forward_frames +
                            c.quotas.remaining_forward_frames - 1;
    if (!cycle.progress)
        s.tail.progress = 0;
    if (cycle.progress < c.quotas.first_forward_frames) {
        const auto work =
                std::min({ s.input_remaining, s.first_remaining, 64U });
        s.first_remaining -= work;
        const auto frames = work - std::uint32_t(s.first_remaining == 0);
        if (admissions) {
            admissions->first_work_frames = work;
            admissions->first_input_frames = frames;
        }
        const auto records = std::min<std::uint64_t>(
                std::uint64_t(work) * c.quotas.forward_entries_per_frame,
                quarter - cycle.work_cursor);
        if (!plan.forward->execute_ring(
                    ring, cycle.input_head, real, cycle.work_cursor, records))
            return false;
        if (right && !plan.forward->execute_ring(right_ring, cycle.input_head,
                             right->working[0], cycle.work_cursor, records))
            return false;
        cycle.work_cursor += static_cast<std::uint32_t>(records);
        advance(frames);
    }
    if (cycle.progress + 1 >= c.quotas.first_forward_frames &&
            cycle.progress < middle_end) {
        if (cycle.work_cursor == quarter)
            s.first_remaining = c.quotas.first_inverse_frames;
        const auto frames = std::min({ s.input_remaining,
                s.forward_middle_remaining, 64 - s.completed_frames });
        s.forward_middle_remaining -= frames;
        if (admissions)
            admissions->middle_frames = frames;
        const auto records = std::min<std::uint64_t>(
                std::uint64_t(frames) * c.quotas.forward_entries_per_frame,
                plan.work.forward_prefix_end - cycle.work_cursor);
        if (!plan.forward->execute(real, imaginary, plan.cosine, plan.sine,
                    cycle.work_cursor, records))
            return false;
        if (right &&
                !plan.forward->execute(right->working[0], right->working[1],
                        plan.cosine, plan.sine, cycle.work_cursor, records))
            return false;
        for (auto index = cycle.work_cursor;
                index < cycle.work_cursor + records; ++index) {
            if (plan.work.forward.flags[index])
                ++cycle.auxiliary_cursor;
            if (plan.work.forward.indices[index + 1] == 0) {
                cycle.stride /= 2;
                ++cycle.stage_shift;
            }
        }
        cycle.work_cursor += static_cast<std::uint32_t>(records);
        advance(frames);
        if (cycle.work_cursor == plan.work.forward_prefix_end)
            s.forward_middle_remaining = c.quotas.remaining_forward_frames;
    }
    if (cycle.work_cursor >= plan.work.forward_prefix_end &&
            cycle.progress < c.available_forward_frames) {
        const auto frames = std::min({ s.input_remaining, s.forward_remaining,
                64 - s.completed_frames });
        if (admissions)
            admissions->terminal_frames = frames;
        const auto first =
                (cycle.work_cursor - plan.work.forward_prefix_end) / 2;
        const auto records = std::min<std::uint64_t>(
                std::uint64_t(frames) *
                        (c.quotas.forward_entries_per_frame / 2),
                plan.terminal->instructions().size() - first);
        if (!plan.terminal->execute_runtime(real, imaginary, plan.cosine,
                    plan.sine, packed_real, packed_imaginary, first, records))
            return false;
        if (right && !plan.terminal->execute_runtime(right->working[0],
                             right->working[1], plan.cosine, plan.sine,
                             right->history[0].subspan(cycle.history_offset, b),
                             right->history[1].subspan(cycle.history_offset, b),
                             first, records))
            return false;
        cycle.work_cursor += static_cast<std::uint32_t>(2 * records);
        advance(frames);
    }
    if (cycle.progress == c.available_forward_frames) {
        if (!plan.terminal->finish_runtime(real, packed_imaginary))
            return false;
        if (right &&
                !plan.terminal->finish_runtime(right->working[0],
                        right->history[1].subspan(cycle.history_offset, b)))
            return false;
        cycle.stride = b;
        cycle.work_cursor = 0;
        cycle.auxiliary_cursor = 0;
        cycle.stage_shift = 0;
        s.forward_remaining = c.available_forward_frames;
    }
    return true;
}
bool run_forward_stages(const MonoWorkspace::Tier &plan,
        MonoTierRuntime &runtime, std::span<const float> ring,
        ForwardStageAdmissions *admissions) noexcept {
    return run_forward_stages_impl(
            plan, runtime, ring, admissions, nullptr, {});
}
bool run_stereo_forward_stages(StereoWorkspace &workspace, std::size_t tier,
        ForwardStageAdmissions *admissions) noexcept {
    auto &common = workspace.common();
    if (tier >= common.tiers().size() || tier >= workspace.right().size())
        return false;
    return run_forward_stages_impl(common.tiers()[tier], common.runtime()[tier],
            common.input_ring(), admissions, &workspace.right()[tier],
            workspace.right_input_ring());
}
} // namespace ekphore::irc::native

#include <array>
#include <bit>
#include <cmath>
#include <stdexcept>

namespace ekphore::irc::native {
InverseTransform InverseTransform::prepare(std::uint32_t block,
        const FftWork &work, std::span<const std::uint32_t> permutation,
        std::size_t twiddle_extent) {
    const auto &v = work.inverse;
    const auto end = work.inverse_prefix_end;
    if (block < 64 || block > (1U << 21) || !std::has_single_bit(block) ||
            permutation.size() != 2 * block ||
            v.indices.size() != v.flags.size() || end < block / 4 ||
            end >= v.indices.size() || v.indices.size() - end != 2 * block ||
            twiddle_extent < 3 * block / 4 + 1)
        throw std::invalid_argument("invalid inverse transform dimensions");
    InverseTransform result;
    result.block_frames = block;
    result.root_extent = twiddle_extent;
    std::uint32_t stride = block, shift = 0;
    std::size_t auxiliary = 0;
    const auto packet = [block](std::uint64_t p) {
        if (p + 4 > 2 * block)
            throw std::invalid_argument("invalid inverse packet");
        return static_cast<std::uint32_t>(p);
    };
    for (std::size_t i = block / 4; i < end; ++i) {
        if (v.indices[i] < 0)
            throw std::invalid_argument("negative inverse index");
        const auto p = packet(v.indices[i]);
        PrefixInstruction op{ PrefixOperation::complex_pair, p, 0, 0, 0 };
        if (v.flags[i]) {
            if (auxiliary >= v.auxiliary.size() || v.auxiliary[auxiliary] < 0)
                throw std::invalid_argument("missing inverse twiddle");
            const auto base = std::uint64_t(v.auxiliary[auxiliary++]) << shift;
            const auto step = std::uint64_t(1) << shift;
            if (3 * (base + 3 * step) >= twiddle_extent)
                throw std::invalid_argument("inverse twiddle outside plane");
            op = { PrefixOperation::twiddle_pair, p,
                packet(std::uint64_t(p) + stride / 2),
                static_cast<std::uint32_t>(base),
                static_cast<std::uint32_t>(step) };
        } else
            op.second = packet(std::uint64_t(p) + stride);
        result.middle_records.push_back(op);
        // eat the marker too, unlike forward
        if (v.indices[i + 1] == 0) {
            stride /= 2;
            ++shift;
        }
        if (shift > std::bit_width(2 * block) - 3)
            throw std::invalid_argument("inverse stage overflow");
    }
    if (stride != 2 || shift != std::bit_width(2 * block) - 3)
        throw std::invalid_argument("incomplete inverse stages");
    result.terminal_twiddle = 1U << (shift - 1);
    std::vector<bool> covered(block);
    for (std::size_t i = end; i < v.indices.size() - block; i += 2) {
        if (v.indices[i] < 0)
            throw std::invalid_argument("negative inverse terminal index");
        const auto p = packet(v.indices[i]);
        const auto a = permutation[p + 1], b = permutation[p + 3];
        for (auto destination : { a, b }) {
            if (destination < block || destination >= 2 * block ||
                    covered[destination - block])
                throw std::invalid_argument("invalid inverse scatter coverage");
            covered[destination - block] = true;
        }
        result.terminal_records.push_back(
                { p, a - block, b - block, v.flags[i] != 0 });
    }
    return result;
}

bool InverseTransform::execute_middle(std::span<float> real,
        std::span<float> imag, std::span<const float> cosine,
        std::span<const float> sine, std::size_t first,
        std::size_t count) const noexcept {
    if (real.size() < 2 * block_frames || imag.size() < 2 * block_frames ||
            cosine.size() < root_extent || sine.size() < root_extent ||
            first > middle_records.size() ||
            count > middle_records.size() - first)
        return false;
    for (auto op : std::span(middle_records).subspan(first, count)) {
        std::array<float, 4> a, b, c, d;
        for (unsigned l = 0; l < 4; ++l) {
            a[l] = real[op.first + l];
            b[l] = real[op.second + l];
            c[l] = imag[op.first + l];
            d[l] = imag[op.second + l];
        }
        // the 3k packet BEFORE the first
        for (unsigned l = 0; l < 4; ++l) {
            if (op.operation == PrefixOperation::complex_pair) {
                real[op.second + l] = a[l] - b[l];
                imag[op.second + l] = c[l] - d[l];
            } else {
                const auto k = 3 * (op.twiddle_base + l * op.twiddle_step);
                const auto x = a[l] - d[l], y = c[l] + b[l];
                real[op.second + l] = x * cosine[k] - y * sine[k];
                imag[op.second + l] = y * cosine[k] + x * sine[k];
            }
        }
        for (unsigned l = 0; l < 4; ++l) {
            if (op.operation == PrefixOperation::complex_pair) {
                real[op.first + l] = a[l] + b[l];
                imag[op.first + l] = c[l] + d[l];
            } else {
                const auto k = op.twiddle_base + l * op.twiddle_step;
                const auto x = a[l] + d[l], y = c[l] - b[l];
                real[op.first + l] = x * cosine[k] - y * sine[k];
                imag[op.first + l] = y * cosine[k] + x * sine[k];
            }
        }
    }
    return true;
}
bool InverseTransform::scatter(std::span<const float> real,
        std::span<const float> imag, std::span<const float> cosine,
        std::span<const float> sine, std::span<float> ring,
        std::size_t ring_base, std::size_t first,
        std::size_t count) const noexcept {
    if (real.size() < 2 * block_frames || imag.size() < 2 * block_frames ||
            cosine.size() < root_extent || sine.size() < root_extent ||
            ring.size() < block_frames || !std::has_single_bit(ring.size()) ||
            ring_base >= ring.size() || first > terminal_records.size() ||
            count > terminal_records.size() - first)
        return false;
    const auto k = terminal_twiddle;
    for (auto op : std::span(terminal_records).subspan(first, count)) {
        const auto p = op.first;
        float a, b;
        if (op.rotated) {
            const auto t1 = std::fma(real[p + 1] + imag[p + 3], sine[k],
                    (imag[p + 1] - real[p + 3]) * cosine[k]);
            const auto t3 = std::fma(real[p + 1] - imag[p + 3], sine[3 * k],
                    (real[p + 3] + imag[p + 1]) * cosine[3 * k]);
            a = (imag[p] - real[p + 2]) - t1;
            b = (imag[p] + real[p + 2]) - t3;
        } else {
            a = (imag[p] + imag[p + 2]) - (imag[p + 1] + imag[p + 3]);
            b = (real[p + 1] - real[p + 3]) + (imag[p] - imag[p + 2]);
        }
        const auto x = (ring_base + op.offset1) & (ring.size() - 1);
        const auto y = (ring_base + op.offset3) & (ring.size() - 1);
        ring[x] = a + ring[x];
        ring[y] = b + ring[y];
    }
    return true;
}
} // namespace ekphore::irc::native

#include <bit>
#include <stdexcept>

namespace ekphore::irc::native {
InverseGather InverseGather::prepare(std::uint32_t block,
        std::span<const std::int32_t> indices,
        std::span<const std::uint32_t> full,
        std::span<const std::uint32_t> half) {
    if (block < 64 || block > (1U << 21) || !std::has_single_bit(block) ||
            indices.size() < block / 4 || full.size() < block * 2 ||
            half.size() < block)
        throw std::invalid_argument(
                "invalid ordinary inverse gather dimensions");
    InverseGather result;
    result.block_frames = block;
    result.gather_packets.reserve(block / 4);
    for (std::size_t record = 0; record < block / 4; ++record) {
        if (indices[record] < 0 || std::uint64_t(indices[record]) + 4 > block)
            throw std::invalid_argument(
                    "inverse gather packet outside half plane");
        const auto base = static_cast<std::uint32_t>(indices[record]);
        InverseGatherPacket packet{};
        packet.destination = base;
        if (full[base + block] >= block * 2)
            throw std::invalid_argument(
                    "inverse gather full permutation outside extent");
        // can't happen with our permutations but keep it anyway
        const bool zero_mirror = full[base + block] == 0;
        for (std::uint32_t lane = 0; lane < 4; ++lane) {
            packet.positive[lane] = half[base + lane];
            packet.mirrored[lane] =
                    zero_mirror ? 0
                                : half[(2 * block - base - lane) & (block - 1)];
            if (packet.positive[lane] >= block ||
                    packet.mirrored[lane] >= block)
                throw std::invalid_argument(
                        "inverse gather source outside packed spectrum");
        }
        // redirect only the first packet's real mirror to Nyquist at B
        packet.first_mirror_real = record == 0 ? block : packet.mirrored[0];
        result.gather_packets.push_back(packet);
    }
    return result;
}

bool InverseGather::execute(std::span<const float> packed_real,
        std::span<const float> packed_imaginary, std::span<float> real,
        std::span<float> imaginary, std::size_t begin,
        std::size_t count) const noexcept {
    if (packed_real.size() < block_frames + 1 ||
            packed_imaginary.size() < block_frames ||
            real.size() < block_frames * 2 ||
            imaginary.size() < block_frames * 2 ||
            begin > gather_packets.size() ||
            count > gather_packets.size() - begin)
        return false;
    for (const auto &packet : std::span(gather_packets).subspan(begin, count)) {
        std::array<float, 4> positive_real, positive_imaginary, mirror_real,
                mirror_imaginary;
        for (std::size_t lane = 0; lane < 4; ++lane) {
            positive_real[lane] = packed_real[packet.positive[lane]];
            positive_imaginary[lane] = packed_imaginary[packet.positive[lane]];
            mirror_real[lane] = packed_real[lane ? packet.mirrored[lane]
                                                 : packet.first_mirror_real];
            mirror_imaginary[lane] = packed_imaginary[packet.mirrored[lane]];
        }
        // add/subtract only, NOT a normal inverse FFT
        for (std::size_t lane = 0; lane < 4; ++lane) {
            const auto lower = packet.destination + lane,
                       upper = lower + block_frames;
            real[upper] = positive_imaginary[lane] + mirror_imaginary[lane];
            imaginary[upper] = positive_real[lane] - mirror_real[lane];
            real[lower] = positive_imaginary[lane] - mirror_imaginary[lane];
            imaginary[lower] = positive_real[lane] + mirror_real[lane];
        }
    }
    return true;
}
} // namespace ekphore::irc::native

#include <limits>

namespace ekphore::irc::native {
static bool run_inverse_stages_impl(const MonoWorkspace::Tier &plan,
        MonoTierRuntime &runtime, std::span<float> ring,
        InverseStageAdmissions *admissions, StereoRightRuntime *right,
        std::span<float> right_ring) noexcept {
    const auto &c = runtime.config;
    auto &s = runtime.state;
    auto &cycle = s.cycle;
    const auto b = c.block, quarter = b / 4, half = b / 2;
    // clang-format off
    if (!plan.gather || !plan.inverse || b < 64 || b > (1U << 21) ||
            !std::has_single_bit(b) || ring.size() < 2 * b ||
            !std::has_single_bit(ring.size()) || ring.size() >
            std::size_t(std::numeric_limits<std::int32_t>::max()) ||
            cycle.output_head >= ring.size() || cycle.input_head >=
            ring.size() || !c.partitions || cycle.history_slot >=
            c.partitions || s.completed_frames > 64 || cycle.progress <
            c.available_forward_frames || cycle.progress > b ||
            !c.mac_packets_per_frame || !c.quotas.inverse_entries_per_frame ||
            !c.quotas.first_inverse_frames ||
            !c.quotas.remaining_inverse_frames || cycle.work_cursor >
            plan.work.inverse_prefix_end + 2 *
            plan.inverse->terminal().size() || s.mac_cursor > b ||
            s.mac_cursor % 4 || runtime.working[0].size() < 2 * b ||
            runtime.working[1].size() < 2 * b ||
            std::uint64_t(cycle.history_offset) + b >
            runtime.history[0].size() || std::uint64_t(cycle.history_offset) +
            b > runtime.history[1].size() || plan.coefficients[0].size() < b ||
            plan.coefficients[1].size() < b)
        // clang-format on
        return false;
    for (auto plane : runtime.accumulation_and_output)
        if (plane.size() < 2 * b)
            return false;
    if (right) {
        // clang-format off
        if (right_ring.size() != ring.size() || right->working[0].size() < 2 *
                b || right->working[1].size() < 2 * b ||
                std::uint64_t(cycle.history_offset) + b >
                right->history[0].size() ||
                std::uint64_t(cycle.history_offset) + b >
                right->history[1].size() || right->coefficients[0].size() < b ||
                right->coefficients[1].size() < b)
            // clang-format on
            return false;
        for (auto plane : right->accumulation_and_output)
            if (plane.size() < 2 * b)
                return false;
    }
    if (admissions)
        *admissions = {};
    auto real = runtime.working[0], imaginary = runtime.working[1];
    auto xr = runtime.history[0].subspan(cycle.history_offset, b);
    auto xi = runtime.history[1].subspan(cycle.history_offset, b);
    auto hr = std::span(plan.coefficients[0]).first(b),
         hi = std::span(plan.coefficients[1]).first(b);
    auto prior_real = runtime.accumulation_and_output[2].first(b);
    auto prior_imaginary = runtime.accumulation_and_output[3].first(b);
    auto yr = runtime.accumulation_and_output[4],
         yi = runtime.accumulation_and_output[5];
    std::span<float> rr, ri, rxr, rxi, rpr, rpi, ryr, ryi;
    std::span<const float> rhr, rhi;
    if (right) {
        rr = right->working[0];
        ri = right->working[1];
        rxr = right->history[0].subspan(cycle.history_offset, b);
        rxi = right->history[1].subspan(cycle.history_offset, b);
        rhr = right->coefficients[0].first(b);
        rhi = right->coefficients[1].first(b);
        rpr = right->accumulation_and_output[2].first(b);
        rpi = right->accumulation_and_output[3].first(b);
        ryr = right->accumulation_and_output[4];
        ryi = right->accumulation_and_output[5];
    }
    const auto advance = [&](std::uint32_t frames, bool inverse) {
        s.input_remaining -= frames;
        cycle.progress += frames;
        s.completed_frames += frames;
        if (inverse)
            cycle.inverse_remaining -= frames;
    };
    if (cycle.progress < half) {
        const auto frames = std::min({ s.input_remaining, s.mac_remaining,
                64 - s.completed_frames });
        const auto packets = std::min<std::uint64_t>(
                std::uint64_t(frames) * c.mac_packets_per_frame,
                (b - s.mac_cursor) / 4);
        if (!multiply_accumulate(xr, xi, hr, hi, prior_real, prior_imaginary,
                    yr, yi, s.mac_cursor / 4, packets))
            return false;
        if (right && !multiply_accumulate(rxr, rxi, rhr, rhi, rpr, rpi, ryr,
                             ryi, s.mac_cursor / 4, packets))
            return false;
        s.mac_cursor += static_cast<std::uint32_t>(4 * packets);
        s.mac_remaining -= frames;
        advance(frames, false);
        if (admissions)
            admissions->mac_frames = frames;
    }
    if (cycle.progress >= half && cycle.work_cursor < quarter) {
        if (cycle.progress == half) {
            s.mac_cursor = 0;
            if (!finalize_real_endpoints(xr, xi, hr, hi, s.endpoints.ready.dc,
                        s.endpoints.ready.nyquist, yr, yi))
                return false;
            if (right && !finalize_real_endpoints(rxr, rxi, rhr, rhi,
                                 right->endpoints.ready.dc,
                                 right->endpoints.ready.nyquist, ryr, ryi))
                return false;
            s.mac_remaining = c.startup_mac_frames;
        }
        const auto work = std::min({ s.input_remaining, s.first_remaining,
                64 - s.completed_frames });
        // positive until the cursor gets to B/4
        if (!s.first_remaining)
            return false;
        s.first_remaining -= work;
        const auto frames = work - std::uint32_t(s.first_remaining == 0);
        const auto packets = std::min<std::uint64_t>(
                std::uint64_t(work) * c.quotas.inverse_entries_per_frame,
                quarter - cycle.work_cursor);
        if (!plan.gather->execute(yr.first(b + 1), yi.first(b), real, imaginary,
                    cycle.work_cursor, packets))
            return false;
        if (right && !plan.gather->execute(ryr.first(b + 1), ryi.first(b), rr,
                             ri, cycle.work_cursor, packets))
            return false;
        cycle.work_cursor += static_cast<std::uint32_t>(packets);
        advance(frames, true);
        if (admissions) {
            admissions->gather_work_frames = work;
            admissions->gather_input_frames = frames;
        }
    }
    const auto middle_end = half + c.quotas.first_inverse_frames +
                            c.quotas.remaining_inverse_frames - 1;
    if (cycle.progress + 1 >= half + c.quotas.first_inverse_frames &&
            cycle.progress < middle_end) {
        if (cycle.work_cursor == quarter)
            s.first_remaining = c.quotas.first_forward_frames;
        const auto frames = std::min({ s.input_remaining,
                s.inverse_middle_remaining, 64 - s.completed_frames });
        s.inverse_middle_remaining -= frames;
        const auto records = std::min<std::uint64_t>(
                std::uint64_t(frames) * c.quotas.inverse_entries_per_frame,
                plan.work.inverse_prefix_end - cycle.work_cursor);
        // we can reach this phase before the gather cursor reaches B/4, with
        // zero work allowed. Don't calculate a negative middle subrange then
        if (records &&
                !plan.inverse->execute_middle(real, imaginary, plan.cosine,
                        plan.sine, cycle.work_cursor - quarter, records))
            return false;
        if (right && records &&
                !plan.inverse->execute_middle(rr, ri, plan.cosine, plan.sine,
                        cycle.work_cursor - quarter, records))
            return false;
        for (auto index = cycle.work_cursor;
                index < cycle.work_cursor + records; ++index) {
            if (plan.work.inverse.flags[index])
                ++cycle.auxiliary_cursor;
            if (plan.work.inverse.indices[index + 1] == 0) {
                cycle.stride /= 2;
                ++cycle.stage_shift;
            }
        }
        cycle.work_cursor += static_cast<std::uint32_t>(records);
        advance(frames, true);
        if (cycle.work_cursor == plan.work.inverse_prefix_end)
            s.inverse_middle_remaining = c.quotas.remaining_inverse_frames;
        if (admissions)
            admissions->middle_frames = frames;
    }
    if (cycle.progress >= middle_end) {
        const auto frames = std::min({ s.input_remaining,
                cycle.inverse_remaining, 64 - s.completed_frames });
        if (frames && cycle.work_cursor < plan.work.inverse_prefix_end)
            return false;
        const auto first =
                frames ? (cycle.work_cursor - plan.work.inverse_prefix_end) / 2
                       : 0;
        const auto records = std::min<std::uint64_t>(
                std::uint64_t(frames) *
                        (c.quotas.inverse_entries_per_frame / 2),
                plan.inverse->terminal().size() - first);
        if (records &&
                !plan.inverse->scatter(real, imaginary, plan.cosine, plan.sine,
                        ring, cycle.output_head, first, records))
            return false;
        if (right && records &&
                !plan.inverse->scatter(rr, ri, plan.cosine, plan.sine,
                        right_ring, cycle.output_head, first, records))
            return false;
        cycle.work_cursor += static_cast<std::uint32_t>(2 * records);
        advance(frames, true);
        if (admissions)
            admissions->terminal_frames = frames;
        if (cycle.progress == b) {
            if (!finish_inverse_cycle(cycle, b,
                        static_cast<std::uint32_t>(ring.size()), c.partitions,
                        imaginary))
                return false;
            if (right)
                std::fill_n(ri.begin(), 2 * b, 0.F);
            if (admissions)
                admissions->cycle_completed = true;
        }
    }
    return true;
}
bool run_inverse_stages(const MonoWorkspace::Tier &plan,
        MonoTierRuntime &runtime, std::span<float> ring,
        InverseStageAdmissions *admissions) noexcept {
    return run_inverse_stages_impl(
            plan, runtime, ring, admissions, nullptr, {});
}
bool run_stereo_inverse_stages(StereoWorkspace &workspace, std::size_t tier,
        InverseStageAdmissions *admissions) noexcept {
    auto &common = workspace.common();
    if (tier >= common.tiers().size() || tier >= workspace.right().size())
        return false;
    return run_inverse_stages_impl(common.tiers()[tier], common.runtime()[tier],
            common.output_ring(), admissions, &workspace.right()[tier],
            workspace.right_output_ring());
}
} // namespace ekphore::irc::native

#include <array>
#include <bit>
#include <cmath>

namespace ekphore::irc::native {
bool multiply_accumulate(std::span<const float> xr, std::span<const float> xi,
        std::span<const float> hr, std::span<const float> hi,
        std::span<const float> pr, std::span<const float> pi,
        std::span<float> yr, std::span<float> yi, std::size_t first,
        std::size_t count) noexcept {
    const auto b = xr.size();
    if (b < 64 || !std::has_single_bit(b) || xi.size() != b || hr.size() != b ||
            hi.size() != b || pr.size() != b || pi.size() != b ||
            yr.size() < b || yi.size() < b || first > b / 4 ||
            count > b / 4 - first)
        return false;
    for (std::size_t packet = first; packet < first + count; ++packet) {
        std::array<float, 4> real, imaginary;
        for (unsigned lane = 0; lane < 4; ++lane) {
            const auto k = 4 * packet + lane;
            const float a = xr[k] * hr[k], b = xi[k] * hi[k];
            const float c = xr[k] * hi[k], d = xi[k] * hr[k];
            real[lane] = (a - b) + pr[k];
            imaginary[lane] = (d + c) + pi[k];
        }
        for (unsigned lane = 0; lane < 4; ++lane)
            yr[4 * packet + lane] = real[lane];
        for (unsigned lane = 0; lane < 4; ++lane)
            yi[4 * packet + lane] = imaginary[lane];
    }
    return true;
}
bool finalize_real_endpoints(std::span<const float> xr,
        std::span<const float> xi, std::span<const float> hr,
        std::span<const float> hi, float dc, float nyquist, std::span<float> yr,
        std::span<float> yi) noexcept {
    const auto b = xr.size();
    if (b < 64 || !std::has_single_bit(b) || xi.size() != b || hr.size() != b ||
            hi.size() != b || yr.size() <= b || yi.size() < b)
        return false;
    const float dc_product = xr[0] * hr[0], nyquist_product = xi[0] * hi[0];
    yr[0] = dc_product + dc;
    yr[b] = nyquist_product + nyquist;
    yi[0] = 0.F;
    return true;
}
bool accumulate_history(std::span<const float> xr, std::span<const float> xi,
        std::span<const float> hr, std::span<const float> hi,
        std::span<float> ar, std::span<float> ai, std::span<float> ready_r,
        std::span<float> ready_i, bool publish, std::size_t first,
        std::size_t count) noexcept {
    const auto b = xr.size();
    if (b < 64 || !std::has_single_bit(b) || xi.size() != b || hr.size() != b ||
            hi.size() != b || ar.size() != b || ai.size() != b ||
            (publish && (ready_r.size() < b || ready_i.size() < b)) ||
            first > b / 4 || count > b / 4 - first)
        return false;
    for (std::size_t packet = first; packet < first + count; ++packet) {
        std::array<float, 4> real, imaginary;
        for (unsigned lane = 0; lane < 4; ++lane) {
            const auto k = 4 * packet + lane;
            const float a = xr[k] * hr[k], b = xi[k] * hi[k];
            const float c = xr[k] * hi[k], d = xi[k] * hr[k];
            real[lane] = ar[k] + (a - b);
            imaginary[lane] = (d + c) + ai[k];
        }
        if (publish) {
            for (unsigned lane = 0; lane < 4; ++lane)
                ready_r[4 * packet + lane] = real[lane];
            for (unsigned lane = 0; lane < 4; ++lane)
                ready_i[4 * packet + lane] = imaginary[lane];
            for (unsigned lane = 0; lane < 4; ++lane)
                ar[4 * packet + lane] = 0.F;
            for (unsigned lane = 0; lane < 4; ++lane)
                ai[4 * packet + lane] = 0.F;
        } else {
            for (unsigned lane = 0; lane < 4; ++lane)
                ar[4 * packet + lane] = real[lane];
            for (unsigned lane = 0; lane < 4; ++lane)
                ai[4 * packet + lane] = imaginary[lane];
        }
    }
    return true;
}
RealEndpoints accumulate_history_endpoints(RealEndpoints prior, float xdc,
        float xnyquist, float hdc, float hnyquist) noexcept {
    return { std::fma(xdc, hdc, prior.dc),
        std::fma(xnyquist, hnyquist, prior.nyquist) };
}
} // namespace ekphore::irc::native

namespace ekphore::irc::native {
bool reset_mono_runtime(MonoRingIoState &global, std::span<float> input_ring,
        std::span<float> output_ring,
        std::span<MonoTierRuntime> tiers) noexcept {
    const auto ring = input_ring.size();
    if (tiers.empty() || tiers.size() > 7 || tiers.front().config.block != 64 ||
            ring != std::uint64_t(tiers.back().config.block) * 4 ||
            ring > 0x7fffffffULL || output_ring.size() != ring ||
            !std::has_single_bit(ring))
        return false;
    std::uint64_t offset = 0;
    for (const auto &tier : tiers) {
        const auto &c = tier.config;
        if (c.block < 64 || c.block > (1U << 21) ||
                !std::has_single_bit(c.block) || c.block > ring / 4 ||
                c.partitions < 2 || c.ir_offset != offset ||
                std::uint64_t(c.block) * c.partitions > 0x7fffffffULL ||
                !c.available_forward_frames ||
                c.available_forward_frames > c.block / 2 ||
                !c.startup_mac_frames || c.startup_mac_frames > c.block / 2 ||
                c.tail.block != c.block || c.tail.partitions != c.partitions ||
                !c.tail.rounds_per_cycle ||
                c.tail.rounds_per_cycle > 0x7fffffffU ||
                offset + 128 < c.block || offset + 128 - c.block >= ring)
            return false;
        for (auto plane : tier.accumulation_and_output)
            if (plane.size() != 2 * c.block)
                return false;
        for (auto plane : tier.working)
            if (plane.size() != 2 * c.block)
                return false;
        for (auto plane : tier.history)
            if (plane.size() != std::uint64_t(c.block) * c.partitions)
                return false;
        offset += std::uint64_t(c.block) * c.partitions;
        if (offset > 0x7fffffffULL)
            return false;
    }
    global = {};
    std::fill(input_ring.begin(), input_ring.end(), 0.F);
    std::fill(output_ring.begin(), output_ring.end(), 0.F);
    for (auto &tier : tiers) {
        const auto &c = tier.config;
        auto &s = tier.state;
        s.cycle = { 0, c.block, 0, 0, c.ir_offset + 128 - c.block, 0,
            static_cast<std::uint32_t>(ring) - 2 * c.block, c.partitions - 1,
            (c.partitions - 1) * c.block, c.block / 2 };
        s.tail = { 0, 1, c.partitions - 2, c.tail.rounds_per_cycle };
        s.endpoints = {};
        s.completed_frames = 0;
        s.host_offset = 0;
        s.mac_cursor = 0;
        s.forward_remaining = c.available_forward_frames;
        s.first_remaining = c.quotas.first_forward_frames;
        s.forward_middle_remaining = c.quotas.remaining_forward_frames;
        s.inverse_middle_remaining = c.quotas.remaining_inverse_frames;
        s.mac_remaining = c.startup_mac_frames;
        for (auto plane : tier.accumulation_and_output)
            std::fill(plane.begin(), plane.end(), 0.F);
        for (auto plane : tier.history)
            std::fill(plane.begin(), plane.end(), 0.F);
        for (auto plane : tier.working)
            std::fill(plane.begin(), plane.end(), 0.F);
    }
    return true;
}
} // namespace ekphore::irc::native

#include <bit>
#include <cmath>
#include <limits>

namespace ekphore::irc::native {
DirectCoefficients prepare_direct_coefficients(
        std::span<const float, 128> ir) noexcept {
    DirectCoefficients result;
    for (unsigned packet = 0; packet < 32; ++packet)
        for (unsigned lane = 0; lane < 4; ++lane)
            result.packets[packet][lane] = ir[packet + 32 * lane];
    result.packets[0][0] = 0.F;
    result.packets[1][0] = 0.F;
    result.sum = (ir[0] + ir[1]) * .5F;
    result.difference = (ir[0] - ir[1]) * .5F;
    return result;
}
static float service_channel(float sample, std::uint32_t head,
        std::uint32_t direct_head, const DirectCoefficients &coefficients,
        DirectPackets &queue, float &sum, float &difference,
        std::span<float> input_ring, std::span<float> output_ring) noexcept {
    input_ring[head] = sample;
    for (unsigned packet = 0; packet < 32; ++packet) {
        auto &destination = queue[(direct_head + packet) & 31];
        for (unsigned lane = 0; lane < 4; ++lane) {
            const float product = coefficients.packets[packet][lane] * sample;
            destination[lane] = product + destination[lane];
        }
    }
    auto &current = queue[direct_head];
    const float direct = current[0];
    current = { current[1], current[2], current[3], 0.F };
    const auto previous =
            (head + input_ring.size() - 1) & (input_ring.size() - 1);
    sum = input_ring[previous] + sample;
    difference = input_ring[previous] - sample;
    const float combined = std::fma(sum, coefficients.sum, output_ring[head]);
    const float boundary =
            std::fma(-difference, coefficients.difference, combined);
    return boundary + direct;
}
bool service_mono_ring(MonoRingIoState &state,
        const DirectCoefficients &coefficients, std::span<const float> input,
        std::span<float> output, std::span<float> mirror,
        std::span<float> input_ring, std::span<float> output_ring) noexcept {
    const auto n = input_ring.size();
    if (n < 256 || n > std::size_t(std::numeric_limits<std::int32_t>::max()) ||
            !std::has_single_bit(n) || output_ring.size() != n ||
            state.ring_head >= n || state.direct_head >= 32 ||
            output.size() < input.size() ||
            (!mirror.empty() && mirror.size() < input.size()))
        return false;
    for (std::size_t frame = 0; frame < input.size(); ++frame) {
        const float result = service_channel(input[frame], state.ring_head,
                state.direct_head, coefficients, state.direct_queue,
                state.boundary_sum, state.boundary_difference, input_ring,
                output_ring);
        output[frame] = result;
        state.direct_head = (state.direct_head + 1) & 31;
        if (!mirror.empty())
            mirror[frame] = result;
        output_ring[state.ring_head] = 0.F;
        state.ring_head = (state.ring_head + 1) & (n - 1);
    }
    return true;
}
bool service_stereo_ring(StereoWorkspace &workspace,
        std::span<const float> left, std::span<const float> right,
        std::span<float> left_output, std::span<float> right_output) noexcept {
    auto &common = workspace.common();
    auto &state = common.ring_state();
    const auto n = common.input_ring().size();
    if (right.size() != left.size() || left_output.size() < left.size() ||
            right_output.size() < right.size() || n < 256 ||
            !std::has_single_bit(n) ||
            n > std::size_t(std::numeric_limits<std::int32_t>::max()) ||
            common.output_ring().size() != n ||
            workspace.right_input_ring().size() != n ||
            workspace.right_output_ring().size() != n || state.ring_head >= n ||
            state.direct_head >= 32)
        return false;
    auto boundary = workspace.right_boundary();
    for (std::size_t frame = 0; frame < left.size(); ++frame) {
        // both inputs BEFORE writing, so in-place mono still works
        const float left_sample = left[frame], right_sample = right[frame];
        left_output[frame] =
                service_channel(left_sample, state.ring_head, state.direct_head,
                        common.direct_coefficients(), state.direct_queue,
                        state.boundary_sum, state.boundary_difference,
                        common.input_ring(), common.output_ring());
        right_output[frame] = service_channel(right_sample, state.ring_head,
                state.direct_head, workspace.right_direct_coefficients(),
                workspace.right_direct_queue(), boundary[0], boundary[1],
                workspace.right_input_ring(), workspace.right_output_ring());
        common.output_ring()[state.ring_head] = 0.F;
        workspace.right_output_ring()[state.ring_head] = 0.F;
        state.direct_head = (state.direct_head + 1) & 31;
        state.ring_head = (state.ring_head + 1) & (n - 1);
    }
    return true;
}
} // namespace ekphore::irc::native

#include <stdexcept>

namespace ekphore::irc::native {
StereoWorkspace StereoWorkspace::clone() const {
    StereoWorkspace result(shared_workspace.clone());
    result.right_storage = right_storage;
    result.right_runtime = right_runtime;
    result.right_input_storage = right_input_storage;
    result.right_output_storage = right_output_storage;
    result.right_direct = right_direct;
    result.right_queue = right_queue;
    result.right_boundary_values = right_boundary_values;
    for (std::size_t i = 0; i < result.right_storage.size(); ++i) {
        auto &storage = result.right_storage[i];
        auto &runtime = result.right_runtime[i];
        for (unsigned k = 0; k < 6; ++k)
            runtime.accumulation_and_output[k] =
                    storage.accumulation_and_output[k];
        for (unsigned k = 0; k < 2; ++k) {
            runtime.history[k] = storage.history[k];
            runtime.working[k] = storage.working[k];
            runtime.coefficients[k] = storage.coefficients[k];
        }
    }
    return result;
}
StereoWorkspace StereoWorkspace::prepare(
        std::span<const float> left, std::span<const float> right) {
    if (left.empty() || left.size() != right.size())
        throw std::invalid_argument(
                "stereo IR requires equal positive channel lengths");
    StereoWorkspace result(MonoWorkspace::prepare(left));
    std::vector<float> padded(
            result.shared_workspace.plan().capacity_frames, 0.F);
    std::copy_n(right.begin(), std::min(right.size(), padded.size()),
            padded.begin());
    result.right_direct = prepare_direct_coefficients(
            std::span<const float, 128>(padded.data(), 128));
    const auto tiers = result.shared_workspace.tiers();
    result.right_storage.resize(tiers.size());
    result.right_runtime.resize(tiers.size());
    for (unsigned index = 0; index < tiers.size(); ++index) {
        const auto &plan = tiers[index];
        const auto &config = result.shared_workspace.runtime()[index].config;
        const auto b = config.block, p = config.partitions;
        auto &storage = result.right_storage[index];
        auto &runtime = result.right_runtime[index];
        for (unsigned k = 0; k < 6; ++k) {
            storage.accumulation_and_output[k].resize(2 * b);
            runtime.accumulation_and_output[k] =
                    storage.accumulation_and_output[k];
        }
        for (unsigned k = 0; k < 2; ++k) {
            storage.history[k].resize(std::size_t(b) * p);
            runtime.history[k] = storage.history[k];
            storage.working[k].resize(2 * b);
            runtime.working[k] = storage.working[k];
            storage.coefficients[k].resize(std::size_t(b) * p);
            runtime.coefficients[k] = storage.coefficients[k];
        }
        for (unsigned partition = 0; partition < p; ++partition) {
            auto &real = storage.working[0];
            auto &imaginary = storage.working[1];
            std::fill(real.begin(), real.end(), 0.F);
            std::fill(imaginary.begin(), imaginary.end(), 0.F);
            std::copy_n(padded.begin() + 128 + config.ir_offset + partition * b,
                    b, real.begin());
            // the fast transform even for the first one
            if (!plan.forward->execute(real, imaginary, plan.cosine, plan.sine,
                        0, plan.forward->instructions().size()) ||
                    !plan.terminal->execute(real, imaginary, plan.cosine,
                            plan.sine, 0,
                            plan.terminal->instructions().size()) ||
                    !plan.terminal->pack(real, imaginary))
                throw std::logic_error(
                        "ordinary stereo right IR transform mismatch");
            std::copy_n(real.begin(), b,
                    storage.coefficients[0].begin() +
                            std::size_t(partition) * b);
            std::copy_n(imaginary.begin(), b,
                    storage.coefficients[1].begin() +
                            std::size_t(partition) * b);
        }
    }
    result.right_input_storage.resize(
            result.shared_workspace.input_ring().size());
    result.right_output_storage.resize(
            result.shared_workspace.output_ring().size());
    if (!result.reset())
        throw std::logic_error("ordinary stereo reset mismatch");
    return result;
}
bool StereoWorkspace::reset() noexcept {
    if (!shared_workspace.reset())
        return false;
    std::fill(right_input_storage.begin(), right_input_storage.end(), 0.F);
    std::fill(right_output_storage.begin(), right_output_storage.end(), 0.F);
    right_queue = {};
    right_boundary_values = {};
    for (auto &runtime : right_runtime) {
        runtime.endpoints = {};
        for (auto plane : runtime.accumulation_and_output)
            std::fill(plane.begin(), plane.end(), 0.F);
        for (auto plane : runtime.history)
            std::fill(plane.begin(), plane.end(), 0.F);
        for (auto plane : runtime.working)
            std::fill(plane.begin(), plane.end(), 0.F);
    }
    return true;
}
} // namespace ekphore::irc::native

#include <cassert>
#include <limits>

namespace ekphore::irc::native {
static bool validate_workspace(MonoWorkspace &workspace,
        std::span<const float> input, std::span<float> output,
        std::span<float> mirror, StereoWorkspace *stereo,
        std::span<const float> right_input,
        std::span<float> right_output) noexcept {
    auto runtime = workspace.runtime();
    const auto plans = workspace.tiers();
    if (input.size() > std::size_t(std::numeric_limits<std::int32_t>::max()) ||
            output.size() < input.size() ||
            (!mirror.empty() && mirror.size() < input.size()) ||
            runtime.empty() || runtime.size() != plans.size())
        return false;
    if (stereo && (right_input.size() != input.size() ||
                          right_output.size() < input.size() ||
                          stereo->right().size() != runtime.size() ||
                          stereo->right_input_ring().size() !=
                                  workspace.input_ring().size() ||
                          stereo->right_output_ring().size() !=
                                  workspace.output_ring().size()))
        return false;
    // reject half prepared ones BEFORE touching anything
    for (std::size_t ti = 0; ti < runtime.size(); ++ti) {
        const auto &r = runtime[ti];
        const auto &p = plans[ti];
        const auto b = r.config.block;
        if (r.state.host_offset || r.state.completed_frames || !p.forward ||
                !p.terminal || !p.gather || !p.inverse || b < 64 ||
                b > (1U << 21) || !std::has_single_bit(b) ||
                r.config.partitions < 2 ||
                p.coefficients[0].size() !=
                        std::uint64_t(b) * r.config.partitions ||
                p.coefficients[1].size() != p.coefficients[0].size())
            return false;
        for (auto plane : r.accumulation_and_output)
            if (plane.size() != 2 * b)
                return false;
        for (unsigned plane = 0; plane < 2; ++plane)
            if (r.working[plane].size() != 2 * b ||
                    r.history[plane].size() != p.coefficients[0].size())
                return false;
        if (stereo) {
            const auto &right = stereo->right()[ti];
            for (auto plane : right.accumulation_and_output)
                if (plane.size() != 2 * b)
                    return false;
            for (unsigned plane = 0; plane < 2; ++plane)
                if (right.working[plane].size() != 2 * b ||
                        right.history[plane].size() !=
                                p.coefficients[plane].size() ||
                        right.coefficients[plane].size() !=
                                p.coefficients[plane].size())
                    return false;
        }
    }
    return true;
}
template <bool Validate>
static bool process_workspace(MonoWorkspace &workspace,
        std::span<const float> input, std::span<float> output,
        std::span<float> mirror, StereoWorkspace *stereo,
        std::span<const float> right_input,
        std::span<float> right_output) noexcept {
    if constexpr (Validate)
        if (!validate_workspace(workspace, input, output, mirror, stereo,
                    right_input, right_output))
            return false;
    auto runtime = workspace.runtime();
    const auto plans = workspace.tiers();
    for (auto &tier : runtime)
        tier.state.input_remaining = static_cast<std::uint32_t>(input.size());
    while (runtime.front().state.host_offset < input.size()) {
        for (std::size_t ti = 0; ti < runtime.size(); ++ti) {
            auto &tier = runtime[ti];
            const auto &plan = plans[ti];
            auto &s = tier.state;
            if (s.cycle.progress < tier.config.available_forward_frames) {
                if (!(stereo ? run_stereo_forward_stages(*stereo, ti)
                             : run_forward_stages(
                                       plan, tier, workspace.input_ring())))
                    return false;
            }
            if (s.cycle.progress >= tier.config.available_forward_frames) {
                if (!(stereo ? run_stereo_inverse_stages(*stereo, ti)
                             : run_inverse_stages(
                                       plan, tier, workspace.output_ring())))
                    return false;
            }
            const auto b = tier.config.block;
            TailBuffers buffers{ tier.history[0], tier.history[1],
                plan.coefficients[0], plan.coefficients[1],
                tier.accumulation_and_output[0].first(b + 1),
                tier.accumulation_and_output[1].first(b + 1),
                tier.accumulation_and_output[2].first(b),
                tier.accumulation_and_output[3].first(b) };
            if (stereo) {
                auto &right = stereo->right()[ti];
                TailBuffers right_buffers{ right.history[0], right.history[1],
                    right.coefficients[0], right.coefficients[1],
                    right.accumulation_and_output[0].first(b + 1),
                    right.accumulation_and_output[1].first(b + 1),
                    right.accumulation_and_output[2].first(b),
                    right.accumulation_and_output[3].first(b) };
                if (!execute_stereo_tail_batch(tier.config.tail, s.tail,
                            buffers, s.endpoints, right_buffers,
                            right.endpoints, s.completed_frames))
                    return false;
            } else if (!execute_tail_batch(tier.config.tail, s.tail, buffers,
                               s.endpoints, s.completed_frames))
                return false;
        }
        // the first tier goes AFTER all of them, then each moves by its OWN
        // amount
        const auto frames = runtime.front().state.completed_frames;
        const auto offset = runtime.front().state.host_offset;
        if (!frames || frames > input.size() - offset)
            return false; // broken state
        if (stereo) {
            if (!service_stereo_ring(*stereo, input.subspan(offset, frames),
                        right_input.subspan(offset, frames),
                        output.subspan(offset, frames),
                        right_output.subspan(offset, frames)))
                return false;
        } else if (!service_mono_ring(workspace.ring_state(),
                           workspace.direct_coefficients(),
                           input.subspan(offset, frames),
                           output.subspan(offset, frames),
                           mirror.empty() ? mirror
                                          : mirror.subspan(offset, frames),
                           workspace.input_ring(), workspace.output_ring()))
            return false;
        for (auto &tier : runtime) {
            tier.state.host_offset += tier.state.completed_frames;
            tier.state.completed_frames = 0;
        }
    }
    for (auto &tier : runtime)
        tier.state.host_offset = 0;
    return true;
}
bool process_mono_workspace(MonoWorkspace &workspace,
        std::span<const float> input, std::span<float> output,
        std::span<float> mirror) noexcept {
    return process_workspace<true>(
            workspace, input, output, mirror, nullptr, {}, {});
}
bool process_stereo_workspace(StereoWorkspace &workspace,
        std::span<const float> left, std::span<const float> right,
        std::span<float> left_output, std::span<float> right_output) noexcept {
    return process_workspace<true>(workspace.common(), left, left_output, {},
            &workspace, right, right_output);
}
std::pair<float, float> StereoWorkspace::process_sample(
        float left, float right) noexcept {
    float output_left{}, output_right{};
    const bool ok = process_workspace<false>(shared_workspace, { &left, 1 },
            { &output_left, 1 }, {}, this, { &right, 1 }, { &output_right, 1 });
    assert(ok);
    (void)ok;
    return { output_left, output_right };
}
} // namespace ekphore::irc::native

namespace ekphore::irc::native {
static bool execute_tail_batch_impl(const TailWorkPlan &plan,
        TailWorkState &state, TailBuffers buffers, TailEndpoints &endpoints,
        std::uint32_t completed, TailBuffers *right,
        TailEndpoints *right_endpoints) noexcept {
    const auto b = plan.block;
    const auto spectra = std::uint64_t(b) * plan.partitions;
    const auto valid_buffers = [&](const TailBuffers &v) {
        return v.input_real.size() == spectra &&
               v.input_imaginary.size() == spectra &&
               v.ir_real.size() == spectra &&
               v.ir_imaginary.size() == spectra &&
               v.accumulation_real.size() == b + 1 &&
               v.accumulation_imaginary.size() == b + 1 &&
               v.ready_real.size() == b && v.ready_imaginary.size() == b;
    };
    // clang-format off
    if (!valid_tail_state(plan, state) || b > (1U << 21) || spectra >
            0x7fffffffULL || plan.rounds_per_cycle != (plan.partitions - 1) *
            std::uint64_t(b) / (4 * plan.packets_per_round) ||
            plan.publication_threshold != (plan.partitions - 2) *
            std::uint64_t(b) / 4 || !valid_buffers(buffers) || (right &&
            (!right_endpoints || !valid_buffers(*right))))
        // clang-format on
        return false;
    const auto end =
            std::uint64_t(plan.packets_per_round) * plan.rounds_per_cycle;
    const auto done = end + plan.packets_per_round;
    if (state.progress > done ||
            (state.progress == end && state.remaining_frames != 0) ||
            (state.progress < end &&
                    (end - state.progress) / plan.packets_per_round !=
                            state.remaining_frames))
        return false;
    const auto count = admit_tail_frames(plan, state, completed);
    for (std::uint32_t frame = 0; frame < count; ++frame) {
        TailRound round;
        if (!next_tail_round(plan, state, round))
            return false;
        const auto execute = [&](TailBuffers v, TailEndpoints &e) {
            const auto xr = v.input_real.subspan(
                    std::size_t(round.history_slot) * b, b);
            const auto xi = v.input_imaginary.subspan(
                    std::size_t(round.history_slot) * b, b);
            const auto hr =
                    v.ir_real.subspan(std::size_t(round.ir_partition) * b, b);
            const auto hi = v.ir_imaginary.subspan(
                    std::size_t(round.ir_partition) * b, b);
            if (!accumulate_history(xr, xi, hr, hi,
                        v.accumulation_real.first(b),
                        v.accumulation_imaginary.first(b), v.ready_real,
                        v.ready_imaginary, round.publish_packet,
                        round.first_packet, round.packet_count))
                return false;
            if (round.accumulate_endpoints) {
                const auto accumulated = accumulate_history_endpoints(
                        { e.accumulated_dc, v.accumulation_real[b] }, xr[0],
                        xi[0], hr[0], hi[0]);
                e.accumulated_dc = accumulated.dc;
                v.accumulation_real[b] = accumulated.nyquist;
            }
            return true;
        };
        if (!execute(buffers, endpoints) ||
                (right && !execute(*right, *right_endpoints)))
            return false;
    }
    if (finish_tail_batch(plan, state)) {
        endpoints.ready = { endpoints.accumulated_dc,
            buffers.accumulation_real[b] };
        endpoints.accumulated_dc = 0.F;
        buffers.accumulation_real[b] = 0.F;
        buffers.accumulation_imaginary[b] = 0.F;
        if (right) {
            right_endpoints->ready = { right_endpoints->accumulated_dc,
                right->accumulation_real[b] };
            right_endpoints->accumulated_dc = 0.F;
            right->accumulation_real[b] = 0.F;
            right->accumulation_imaginary[b] = 0.F;
        }
    }
    return true;
}
bool execute_tail_batch(const TailWorkPlan &plan, TailWorkState &state,
        TailBuffers buffers, TailEndpoints &endpoints,
        std::uint32_t completed) noexcept {
    return execute_tail_batch_impl(
            plan, state, buffers, endpoints, completed, nullptr, nullptr);
}
bool execute_stereo_tail_batch(const TailWorkPlan &plan, TailWorkState &state,
        TailBuffers left, TailEndpoints &left_endpoints, TailBuffers right,
        TailEndpoints &right_endpoints, std::uint32_t completed) noexcept {
    return execute_tail_batch_impl(plan, state, left, left_endpoints, completed,
            &right, &right_endpoints);
}
} // namespace ekphore::irc::native

#include <cmath>
#include <limits>
#include <stdexcept>

namespace ekphore::irc::native {
std::uint32_t ordinary_startup_frames(std::uint32_t block, bool first) {
    if (block < 64 || block > (1U << 21) || !std::has_single_bit(block))
        throw std::invalid_argument("invalid ordinary startup block");
    if (first)
        return 2;
    volatile float input = static_cast<float>(block);
    volatile float logarithm = std::log(input);
    volatile float adjusted = logarithm - 2.F;
    volatile float squared = adjusted * adjusted;
    return static_cast<std::uint32_t>(squared);
}
MonoWorkspace MonoWorkspace::prepare(std::int32_t frames) {
    return prepare_impl(frames, {});
}
MonoWorkspace MonoWorkspace::clone() const {
    MonoWorkspace result;
    result.partitioning = partitioning;
    result.tier_data = tier_data;
    result.tier_runtime = tier_runtime;
    result.input_storage = input_storage;
    result.output_storage = output_storage;
    result.io_state = io_state;
    result.direct = direct;
    for (std::size_t i = 0; i < result.tier_data.size(); ++i) {
        auto &tier = result.tier_data[i];
        auto &runtime = result.tier_runtime[i];
        for (unsigned k = 0; k < 6; ++k)
            runtime.accumulation_and_output[k] =
                    tier.accumulation_and_output[k];
        for (unsigned k = 0; k < 2; ++k) {
            runtime.history[k] = tier.history[k];
            runtime.working[k] = tier.working[k];
        }
    }
    return result;
}
MonoWorkspace MonoWorkspace::prepare(std::span<const float> ir) {
    if (ir.empty() ||
            ir.size() > std::size_t(std::numeric_limits<std::int32_t>::max()))
        throw std::invalid_argument("invalid ordinary mono IR extent");
    return prepare_impl(static_cast<std::int32_t>(ir.size()), ir);
}
MonoWorkspace MonoWorkspace::prepare_impl(
        std::int32_t frames, std::span<const float> ir) {
    MonoWorkspace result;
    result.partitioning = ordinary_partition_plan(frames);
    std::vector<float> padded;
    if (!ir.empty()) {
        padded.resize(result.partitioning.capacity_frames, 0.F);
        std::copy_n(
                ir.begin(), std::min(ir.size(), padded.size()), padded.begin());
        result.direct = prepare_direct_coefficients(
                std::span<const float, 128>(padded.data(), 128));
    }
    const auto count = result.partitioning.tier_count;
    result.tier_data.resize(count);
    result.tier_runtime.resize(count);
    std::uint32_t offset = 0;
    for (unsigned index = 0; index < count; ++index) {
        const auto b = result.partitioning.tiers[index].block_frames;
        const auto p = result.partitioning.tiers[index].partition_count;
        auto &tier = result.tier_data[index];
        auto &runtime = result.tier_runtime[index];
        tier.permutation.resize(2 * b);
        tier.half_permutation.resize(b);
        fill_bit_reversal(tier.permutation);
        fill_bit_reversal(tier.half_permutation);
        tier.cosine.resize(2 * b);
        tier.sine.resize(2 * b);
        fill_twiddles(tier.cosine, tier.sine);
        for (auto &plane : tier.working)
            plane.resize(2 * b);
        if (ir.empty())
            tier.work = generate_ordinary_fft_work(b);
        else {
            std::copy_n(
                    padded.begin() + 128 + offset, b, tier.working[0].begin());
            tier.work = transform_first_ir_partition(tier.working[0],
                    tier.working[1], tier.cosine, tier.sine, tier.permutation);
        }
        tier.forward = ForwardPrefix::prepare(b, tier.work.forward.indices,
                tier.work.forward.flags, tier.work.forward.auxiliary,
                tier.work.forward_prefix_end, 2 * b);
        tier.terminal = ForwardTerminal::prepare(b, tier.work.forward.indices,
                tier.work.forward.flags, tier.permutation,
                tier.work.forward_prefix_end, 2 * b);
        tier.gather = InverseGather::prepare(b, tier.work.inverse.indices,
                tier.permutation, tier.half_permutation);
        tier.inverse = InverseTransform::prepare(
                b, tier.work, tier.permutation, 2 * b);
        if (!ir.empty()) {
            for (auto &plane : tier.coefficients)
                plane.resize(std::size_t(b) * p);
            for (unsigned partition = 0; partition < p; ++partition) {
                auto &real = tier.working[0];
                auto &imaginary = tier.working[1];
                if (partition) {
                    std::fill(real.begin(), real.end(), 0.F);
                    std::fill(imaginary.begin(), imaginary.end(), 0.F);
                    std::copy_n(padded.begin() + 128 + offset + partition * b,
                            b, real.begin());
                    if (!tier.forward->execute(real, imaginary, tier.cosine,
                                tier.sine, 0,
                                tier.forward->instructions().size()) ||
                            !tier.terminal->execute(real, imaginary,
                                    tier.cosine, tier.sine, 0,
                                    tier.terminal->instructions().size()) ||
                            !tier.terminal->pack(real, imaginary))
                        throw std::logic_error(
                                "ordinary IR transform plan mismatch");
                }
                std::copy_n(real.begin(), b,
                        tier.coefficients[0].begin() +
                                std::size_t(partition) * b);
                std::copy_n(imaginary.begin(), b,
                        tier.coefficients[1].begin() +
                                std::size_t(partition) * b);
            }
        }
        const auto startup = ordinary_startup_frames(b, index == 0);
        const auto available = b / 2 - startup;
        const auto quotas = prepare_stage_quotas({ b,
                static_cast<std::uint32_t>(tier.work.forward.indices.size()),
                static_cast<std::uint32_t>(tier.work.inverse.indices.size()),
                available, tier.work.forward_prefix_end,
                tier.work.inverse_prefix_end });
        runtime.config = { b, p, offset, available, startup, quotas,
            prepare_tail_work(b, p), detail::ceil_stage_ratio(b / 4, startup) };
        offset += b * p;
        for (unsigned k = 0; k < 6; ++k) {
            tier.accumulation_and_output[k].resize(2 * b);
            runtime.accumulation_and_output[k] =
                    tier.accumulation_and_output[k];
        }
        for (unsigned k = 0; k < 2; ++k) {
            tier.history[k].resize(std::size_t(b) * p);
            runtime.history[k] = tier.history[k];
            tier.working[k].resize(2 * b);
            runtime.working[k] = tier.working[k];
        }
    }
    const auto ring = 4 * result.partitioning.tiers[count - 1].block_frames;
    result.input_storage.resize(ring);
    result.output_storage.resize(ring);
    if (!result.reset())
        throw std::logic_error("ordinary workspace reset geometry mismatch");
    return result;
}
bool MonoWorkspace::reset() noexcept {
    return reset_mono_runtime(
            io_state, input_storage, output_storage, tier_runtime);
}
} // namespace ekphore::irc::native

#include <algorithm>
#include <cassert>

namespace ekphore::irc {
StagedPairedConvolver StagedPairedConvolver::clone() const {
    StagedPairedConvolver result;
    result.convolution_rate = convolution_rate;
    result.response_frames = response_frames;
    result.ready = ready;
    if (workspace)
        result.workspace =
                std::make_unique<native::StereoWorkspace>(workspace->clone());
    return result;
}
StagedPairedConvolver StagedPairedConvolver::prepare(
        std::vector<std::vector<float>> channels, unsigned divisor) {
    StagedPairedConvolver result;
    result.convolution_rate = PairedConvolutionRate::prepare(divisor);
    result.ready = !channels.empty();
    for (const auto &channel : channels)
        result.response_frames =
                std::max(result.response_frames, channel.size());
    if (!result.response_frames)
        return result;
    // mono IR goes on both, uneven stereo gets padded and NEVER cross fed
    if (channels.size() == 1)
        channels.push_back(channels.front());
    channels[0].resize(result.response_frames);
    channels[1].resize(result.response_frames);
    result.workspace = std::make_unique<native::StereoWorkspace>(
            native::StereoWorkspace::prepare(channels[0], channels[1]));
    return result;
}
void StagedPairedConvolver::swap(StagedPairedConvolver &other) noexcept {
    convolution_rate.swap(other.convolution_rate);
    std::swap(ready, other.ready);
    std::swap(response_frames, other.response_frames);
    workspace.swap(other.workspace);
}
void StagedPairedConvolver::reset() noexcept {
    convolution_rate.reset();
    if (workspace) {
        const bool ok = workspace->reset();
        assert(ok);
        (void)ok;
    }
}
std::pair<float, float> StagedPairedConvolver::process(
        float left, float right) {
    return convolution_rate.process(
            left, right, [this](float l, float r) -> std::pair<float, float> {
                if (!workspace)
                    return {};
                return workspace->process_sample(l, r);
            });
}
} // namespace ekphore::irc
