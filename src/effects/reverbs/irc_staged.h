// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "../../dsp/math/integer_half_phase.h"
#include <stdexcept>
#include <utility>

namespace ekphore::irc {
// integer before fractional, and the phase carries across host callbacks
class PairedConvolutionRate {
  public:
    static PairedConvolutionRate prepare(unsigned divisor) {
        if (divisor != 1 && divisor != 2)
            throw std::invalid_argument(
                    "unimplemented IRC processing-rate divisor");
        PairedConvolutionRate result;
        result.rate_divisor = divisor;
        if (divisor == 2)
            result.coefficients = dsp::integer_half_phase_coefficients();
        return result;
    }
    unsigned divisor() const noexcept { return rate_divisor; }
    void reset() noexcept {
        fractional_input = false;
        down_fir = {};
        up_fir = {};
        down_delay = {};
        up_delay = {};
        down_fractional = {};
        up_fractional = {};
    }
    void swap(PairedConvolutionRate &other) noexcept {
        std::swap(*this, other);
    }
    template <class Core>
    std::pair<float, float> process(float left, float right, Core &&core) {
        if (rate_divisor == 1)
            return core(left, right);
        const std::array<float, 2> input{ left, right };
        if (fractional_input) {
            for (unsigned c = 0; c < 2; ++c)
                down_fractional[c] =
                        down_fir[c].process(input[c], coefficients);
            fractional_input = false;
            return { up_fractional[0], up_fractional[1] };
        }
        std::array<float, 2> reduced;
        for (unsigned c = 0; c < 2; ++c)
            reduced[c] =
                    (down_delay[c].process(input[c]) + down_fractional[c]) *
                    .5F;
        const auto wet = core(reduced[0], reduced[1]);
        const std::array<float, 2> w{ wet.first, wet.second };
        std::array<float, 2> output;
        for (unsigned c = 0; c < 2; ++c) {
            output[c] = up_delay[c].process(w[c]);
            up_fractional[c] = up_fir[c].process(w[c], coefficients);
        }
        fractional_input = true;
        return { output[0], output[1] };
    }

  private:
    unsigned rate_divisor{ 1 };
    bool fractional_input{};
    std::array<float, 16> coefficients{};
    std::array<dsp::IntegerHalfPhaseFir, 2> down_fir, up_fir;
    std::array<dsp::IntegerPhaseDelay, 2> down_delay, up_delay;
    std::array<float, 2> down_fractional{}, up_fractional{};
};
} // namespace ekphore::irc

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>

namespace ekphore::irc::native {
// you allocate the output
inline void fill_bit_reversal(std::span<std::uint32_t> output) {
    const auto count = output.size();
    if (count < 2 ||
            count > std::size_t(std::numeric_limits<std::int32_t>::max()) ||
            !std::has_single_bit(count))
        throw std::invalid_argument(
                "FFT permutation requires a power-of-two extent of at least2");
    const auto size = static_cast<std::uint32_t>(count);
    std::uint32_t reversed = 0;
    for (std::uint32_t i = 0; i + 1 < size; ++i) {
        output[i] = reversed;
        auto increment = size / 2;
        while (increment <= reversed) {
            reversed -= increment;
            increment /= 2;
        }
        reversed += increment;
    }
    output.back() = size - 1;
}

namespace detail {
// float stores so it rounds the same even if the compiler uses wider floats
inline std::uint32_t ceil_stage_ratio(
        std::uint32_t numerator, std::uint32_t denominator) {
    constexpr auto maximum =
            std::uint32_t(std::numeric_limits<std::int32_t>::max());
    if (!denominator || numerator > maximum || denominator > maximum)
        throw std::invalid_argument("invalid FFT work ratio");
    volatile float n = static_cast<float>(numerator);
    volatile float d = static_cast<float>(denominator);
    volatile float ratio = n / d;
    const double rounded = std::ceil(static_cast<double>(ratio));
    if (rounded > maximum)
        throw std::overflow_error("FFT work quota outside qualified domain");
    return static_cast<std::uint32_t>(rounded);
}
} // namespace detail

// from the generated work vectors, NOT an N*log2(N) guess
struct StageWork {
    std::uint32_t block_frames{};
    std::uint32_t forward_entries{}, inverse_entries{};
    std::uint32_t available_forward_frames{};
    std::uint32_t forward_prefix_end{}, inverse_prefix_end{};
};
struct StageQuotas {
    std::uint32_t forward_entries_per_frame{}, inverse_entries_per_frame{};
    std::uint32_t first_forward_frames{}, first_inverse_frames{};
    std::uint32_t remaining_forward_frames{}, remaining_inverse_frames{};
    bool operator==(const StageQuotas &) const = default;
};
inline StageQuotas prepare_stage_quotas(const StageWork &work) {
    const auto quarter = work.block_frames / 4;
    if (work.block_frames < 64 || !std::has_single_bit(work.block_frames) ||
            work.block_frames >
                    std::uint32_t(std::numeric_limits<std::int32_t>::max()) ||
            !work.forward_entries || !work.inverse_entries ||
            !work.available_forward_frames ||
            work.forward_prefix_end < quarter ||
            work.inverse_prefix_end < quarter ||
            work.forward_prefix_end > work.forward_entries ||
            work.inverse_prefix_end > work.inverse_entries)
        throw std::invalid_argument("invalid split-radix work dimensions");
    const auto forward = detail::ceil_stage_ratio(
            work.forward_entries, work.available_forward_frames);
    const auto inverse = detail::ceil_stage_ratio(
            work.inverse_entries, work.block_frames / 2);
    return { forward, inverse, detail::ceil_stage_ratio(quarter, forward),
        detail::ceil_stage_ratio(quarter, inverse),
        detail::ceil_stage_ratio(work.forward_prefix_end - quarter, forward),
        detail::ceil_stage_ratio(work.inverse_prefix_end - quarter, inverse) };
}

struct ForwardPrefixAdmission {
    std::uint32_t work_frames{}, input_frames{}, remaining_stage_frames{};
    bool operator==(const ForwardPrefixAdmission &) const = default;
};
// the last frame's work gets spent but the frame stays for the next stage
inline ForwardPrefixAdmission admit_forward_prefix(
        std::uint32_t input_remaining, std::uint32_t stage_remaining) {
    constexpr auto maximum =
            std::uint32_t(std::numeric_limits<std::int32_t>::max());
    if (!stage_remaining || input_remaining > maximum ||
            stage_remaining > maximum)
        throw std::invalid_argument("invalid forward-prefix cursor");
    const auto work = std::min({ input_remaining, stage_remaining, 64U });
    const auto remaining = stage_remaining - work;
    return { work, work - std::uint32_t(remaining == 0), remaining };
}
} // namespace ekphore::irc::native

#include <algorithm>
#include <bit>
#include <cstdint>
#include <limits>
#include <span>

namespace ekphore::irc::native {
// the live cursors, NOT the defaults
struct InverseCycleState {
    std::uint32_t progress{};
    std::uint32_t stride{};
    std::uint32_t work_cursor{};
    std::uint32_t auxiliary_cursor{};
    std::uint32_t output_head{};
    std::uint32_t stage_shift{};
    std::uint32_t input_head{};
    std::uint32_t history_slot{};
    std::uint32_t history_offset{};
    std::uint32_t inverse_remaining{};
    bool operator==(const InverseCycleState &) const = default;
};
// changes nothing if it fails
inline bool finish_inverse_cycle(InverseCycleState &state, std::uint32_t block,
        std::uint32_t ring_frames, std::uint32_t history_slots,
        std::span<float> working_imaginary) noexcept {
    // clang-format off
    if (block < 64 || block > (1U << 21) || !std::has_single_bit(block) ||
            ring_frames < 2 * block || !std::has_single_bit(ring_frames) ||
            ring_frames >
            std::uint32_t(std::numeric_limits<std::int32_t>::max()) ||
            !history_slots || std::uint64_t(history_slots) * block >
            std::uint32_t(std::numeric_limits<std::int32_t>::max()) ||
            state.progress != block || state.history_slot >= history_slots ||
            state.output_head >= ring_frames || state.input_head >=
            ring_frames || working_imaginary.size() < 2 * block)
        // clang-format on
        return false;
    state.progress = 0;
    state.stride = block;
    state.work_cursor = 0;
    state.auxiliary_cursor = 0;
    state.output_head = (state.output_head + block) & (ring_frames - 1);
    state.stage_shift = 0;
    state.input_head = (state.input_head + block) & (ring_frames - 1);
    state.history_slot = (state.history_slot + 1) % history_slots;
    std::fill_n(working_imaginary.begin(), 2 * block, 0.F);
    state.history_offset = state.history_slot * block;
    state.inverse_remaining = block / 2;
    return true;
}
} // namespace ekphore::irc::native

#include <array>
#include <cstdint>
#include <span>

namespace ekphore::irc::native {
class StereoWorkspace;
using DirectPackets = std::array<std::array<float, 4>, 32>;
struct DirectCoefficients {
    DirectPackets packets{};
    float sum{}, difference{};
};
// first 128 taps as four lanes of 32, taps 0/1 are zeroed and done separately
// with half sum/difference
DirectCoefficients prepare_direct_coefficients(
        std::span<const float, 128> ir) noexcept;
struct MonoRingIoState {
    std::uint32_t ring_head{}, direct_head{};
    float boundary_sum{}, boundary_difference{};
    DirectPackets direct_queue{};
};
// input and output can be the same buffer, the mirror is empty or at least as
// big as the input
bool service_mono_ring(MonoRingIoState &, const DirectCoefficients &,
        std::span<const float> input, std::span<float> output,
        std::span<float> mirror, std::span<float> input_ring,
        std::span<float> output_ring) noexcept;
bool service_stereo_ring(StereoWorkspace &, std::span<const float> left_input,
        std::span<const float> right_input, std::span<float> left_output,
        std::span<float> right_output) noexcept;
} // namespace ekphore::irc::native

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace ekphore::irc::native {
struct TailWorkPlan {
    std::uint32_t block{}, partitions{}, packets_per_round{};
    std::uint32_t rounds_per_cycle{};
    std::uint32_t publication_threshold{};
};
// positive int32 only
inline TailWorkPlan prepare_tail_work(
        std::uint32_t block, std::uint32_t partitions) {
    constexpr auto maximum =
            std::uint32_t(std::numeric_limits<std::int32_t>::max());
    if (block < 64 || block > (1U << 21) || !std::has_single_bit(block) ||
            partitions < 2 || std::uint64_t(partitions) * block > maximum)
        throw std::invalid_argument("invalid ordinary tail work dimensions");
    volatile float count = static_cast<float>(partitions - 1);
    volatile float quarter = count * .25F;
    const auto quota =
            std::bit_ceil(static_cast<std::uint32_t>(std::ceil(quarter)));
    // whole four bin packets only, TODO: the others
    if (quota > block / 4)
        throw std::invalid_argument(
                "tail quota outside qualified packet domain");
    return { block, partitions, quota, (partitions - 1) * block / (4 * quota),
        (partitions - 2) * block / 4 };
}
struct TailWorkState {
    std::uint32_t progress{};
    std::uint32_t history_slot{};
    std::uint32_t coefficient_slot{}; // not counting the first partition
    std::uint32_t remaining_frames{};
    bool operator==(const TailWorkState &) const = default;
};
struct TailRound {
    std::uint32_t first_packet{}, packet_count{}, history_slot{},
            ir_partition{};
    bool publish_packet{}, accumulate_endpoints{};
};
inline bool valid_tail_state(
        const TailWorkPlan &plan, const TailWorkState &state) noexcept {
    return plan.block >= 64 && plan.partitions >= 2 && plan.packets_per_round &&
           std::has_single_bit(plan.block) &&
           std::has_single_bit(plan.packets_per_round) &&
           plan.packets_per_round <= plan.block / 4 &&
           state.history_slot < plan.partitions &&
           state.coefficient_slot < plan.partitions - 1 &&
           state.remaining_frames <= plan.rounds_per_cycle &&
           state.progress % plan.packets_per_round == 0;
}
// at most min(completed frames, remaining, 64) rounds
inline std::uint32_t admit_tail_frames(const TailWorkPlan &plan,
        TailWorkState &state, std::uint32_t completed_frames) noexcept {
    const auto stop = std::uint64_t(plan.packets_per_round) *
                      (plan.rounds_per_cycle + 1ULL);
    if (!valid_tail_state(plan, state) || state.progress >= stop)
        return 0;
    const auto count =
            std::min({ completed_frames, state.remaining_frames, 64U });
    state.remaining_frames -= count;
    return count;
}
// describes the work BEFORE advancing, you run it yourself
inline bool next_tail_round(const TailWorkPlan &plan, TailWorkState &state,
        TailRound &round) noexcept {
    const auto end =
            std::uint64_t(plan.packets_per_round) * plan.rounds_per_cycle;
    if (!valid_tail_state(plan, state) || state.progress >= end)
        return false;
    const auto next = state.progress + plan.packets_per_round;
    round = { state.progress & (plan.block / 4 - 1), plan.packets_per_round,
        state.history_slot, state.coefficient_slot + 1,
        state.progress >= plan.publication_threshold,
        (next & (plan.block / 4 - 1)) == 0 };
    state.progress = next;
    if (round.accumulate_endpoints) {
        state.history_slot = (state.history_slot + 1) % plan.partitions;
        state.coefficient_slot =
                (state.coefficient_slot + plan.partitions - 2) %
                (plan.partitions - 1);
    }
    return true;
}
// AFTER the whole batch, NOT between rounds. true means publish the endpoints
inline bool finish_tail_batch(
        const TailWorkPlan &plan, TailWorkState &state) noexcept {
    const auto end =
            std::uint64_t(plan.packets_per_round) * plan.rounds_per_cycle;
    if (!valid_tail_state(plan, state) || state.progress != end)
        return false;
    state.remaining_frames = plan.rounds_per_cycle;
    state.progress += plan.packets_per_round;
    state.history_slot = (state.history_slot + 2) % plan.partitions;
    return true;
}
} // namespace ekphore::irc::native

#include <span>

namespace ekphore::irc::native {
// four bin packets, source and destination are separate buffers, NOT in place
bool multiply_accumulate(std::span<const float> input_real,
        std::span<const float> input_imaginary, std::span<const float> ir_real,
        std::span<const float> ir_imaginary, std::span<const float> prior_real,
        std::span<const float> prior_imaginary, std::span<float> output_real,
        std::span<float> output_imaginary, std::size_t first_packet,
        std::size_t packet_count) noexcept;
// DC and Nyquist share slot 0 but they're separate, real output needs B+1
bool finalize_real_endpoints(std::span<const float> input_real,
        std::span<const float> input_imaginary, std::span<const float> ir_real,
        std::span<const float> ir_imaginary, float prior_dc,
        float prior_nyquist, std::span<float> output_real,
        std::span<float> output_imaginary) noexcept;

// later partitions just pile up into history, when we publish it goes
// to ready and history gets zeroed, the scheduler tells us when
bool accumulate_history(std::span<const float> input_real,
        std::span<const float> input_imaginary, std::span<const float> ir_real,
        std::span<const float> ir_imaginary, std::span<float> history_real,
        std::span<float> history_imaginary, std::span<float> ready_real,
        std::span<float> ready_imaginary, bool publish,
        std::size_t first_packet, std::size_t packet_count) noexcept;
struct RealEndpoints {
    float dc{}, nyquist{};
};
// fused here, unlike the first partition
RealEndpoints accumulate_history_endpoints(RealEndpoints prior, float input_dc,
        float input_nyquist, float ir_dc, float ir_nyquist) noexcept;
} // namespace ekphore::irc::native

namespace ekphore::irc::native {
struct TailBuffers {
    std::span<const float> input_real, input_imaginary, ir_real, ir_imaginary;
    std::span<float> accumulation_real, accumulation_imaginary, ready_real,
            ready_imaginary;
};
struct TailEndpoints {
    float accumulated_dc{};
    RealEndpoints ready{};
};
// P spectra of B bins, accumulation is B+1 for Nyquist. reset progress
// yourself on a new transform
bool execute_tail_batch(const TailWorkPlan &, TailWorkState &, TailBuffers,
        TailEndpoints &, std::uint32_t completed_frames) noexcept;
bool execute_stereo_tail_batch(const TailWorkPlan &, TailWorkState &,
        TailBuffers left, TailEndpoints &left_endpoints, TailBuffers right,
        TailEndpoints &right_endpoints,
        std::uint32_t completed_frames) noexcept;
} // namespace ekphore::irc::native

namespace ekphore::irc::native {
struct TierResetConfig {
    std::uint32_t block{}, partitions{}, ir_offset{};
    std::uint32_t available_forward_frames{}, startup_mac_frames{};
    StageQuotas quotas{};
    TailWorkPlan tail{};
    std::uint32_t mac_packets_per_frame{}; // so we don't divide at runtime
};
struct MonoTierState {
    InverseCycleState cycle{};
    TailWorkState tail{};
    TailEndpoints endpoints{};
    std::uint32_t completed_frames{}, host_offset{}, mac_cursor{};
    std::uint32_t forward_remaining{}, first_remaining{},
            forward_middle_remaining{};
    std::uint32_t inverse_middle_remaining{}, mac_remaining{};
    std::uint32_t input_remaining{}; // reset doesn't assign this
};
struct MonoTierRuntime {
    TierResetConfig config{};
    MonoTierState state{};
    // 2B floats each
    std::array<std::span<float>, 6> accumulation_and_output;
    // P*B for histories, 2B for working
    std::array<std::span<float>, 2> history, working;
};
// has to be fully prepared, checks everything BEFORE writing
bool reset_mono_runtime(MonoRingIoState &, std::span<float> input_ring,
        std::span<float> output_ring,
        std::span<MonoTierRuntime> tiers) noexcept;
} // namespace ekphore::irc::native

// 283 records of 64 bytes

#include <array>
#include <cstddef>
#include <cstdint>

namespace ekphore::irc::native {
struct PartitionTier {
    std::uint32_t block_frames{};
    std::uint32_t partition_count{};
    bool operator==(const PartitionTier &) const = default;
};

// same layout, but each backend has its own table
struct PartitionPlan {
    std::uint32_t capacity_frames{};
    std::uint32_t tier_count{};
    std::array<PartitionTier, 7> tiers{};
    bool operator==(const PartitionPlan &) const = default;
};
static_assert(sizeof(PartitionPlan) == 64);
static_assert(offsetof(PartitionPlan, tiers) == 8);
} // namespace ekphore::irc::native

namespace ekphore::irc::native::detail {
#include "irc_ordinary_plans.inc"
} // namespace ekphore::irc::native::detail

namespace ekphore::irc::native::detail {
#include "irc_fft_plans.inc"

} // namespace ekphore::irc::native::detail

#include <algorithm>
#include <limits>
#include <span>
#include <stdexcept>

namespace ekphore::irc::native {
inline constexpr std::uint32_t ordinary_direct_head_frames = 128;
enum class Backend { ordinary, latency, platform_fft };

constexpr bool valid_partition_table(
        std::span<const PartitionPlan> plans, std::uint32_t head_frames) {
    std::uint32_t previous = 0;
    for (const auto &plan : plans) {
        // some capacities show up twice, the first one wins
        if (!plan.capacity_frames || plan.capacity_frames < previous ||
                !plan.tier_count || plan.tier_count > plan.tiers.size())
            return false;
        std::uint64_t capacity = head_frames;
        std::uint32_t previous_block = 0;
        for (std::size_t i = 0; i < plan.tiers.size(); ++i) {
            const auto &tier = plan.tiers[i];
            if (i >= plan.tier_count) {
                if (tier.block_frames || tier.partition_count)
                    return false;
                continue;
            }
            // equal tiers next to each other are separate buffers, don't
            // merge them
            if (tier.block_frames < 64 || tier.block_frames < previous_block ||
                    (tier.block_frames & (tier.block_frames - 1)) ||
                    !tier.partition_count)
                return false;
            capacity += std::uint64_t(tier.block_frames) * tier.partition_count;
            previous_block = tier.block_frames;
        }
        if (capacity != plan.capacity_frames)
            return false;
        previous = plan.capacity_frames;
    }
    return true;
}
static_assert(valid_partition_table(
        detail::partition_plans, ordinary_direct_head_frames));
static_assert(valid_partition_table(detail::platform64_plans, 0));
static_assert(valid_partition_table(detail::platform128_plans, 0));
static_assert(valid_partition_table(detail::platform256_plans, 0));
static_assert(valid_partition_table(detail::platform512_plans, 0));
static_assert(valid_partition_table(detail::platform1024_plans, 0));
static_assert(valid_partition_table(detail::platform2048_plans, 0));
static_assert(valid_partition_table(detail::platform4096_plans, 0));
static_assert(valid_partition_table(detail::latency2048_plans, 0));
static_assert(valid_partition_table(detail::latency4096_plans, 0));

constexpr std::span<const PartitionPlan> partition_table(
        Backend backend, std::int32_t authored_block) {
    if (backend == Backend::ordinary && authored_block >= 0 &&
            authored_block <= 64)
        return detail::partition_plans;
    if (backend == Backend::latency) {
        // only identical tables get shared, 2048/4096 have extra records
        switch (authored_block) {
            case 128:
                return detail::platform64_plans;
            case 256:
                return detail::platform128_plans;
            case 512:
                return detail::platform256_plans;
            case 1024:
                return detail::platform512_plans;
            case 2048:
                return detail::latency2048_plans;
            case 4096:
                return detail::latency4096_plans;
        }
    }
    if (backend == Backend::platform_fft) {
        switch (authored_block) {
            case 0:
            case 64:
                return detail::platform64_plans;
            case 128:
                return detail::platform128_plans;
            case 256:
                return detail::platform256_plans;
            case 512:
                return detail::platform512_plans;
            case 1024:
                return detail::platform1024_plans;
            case 2048:
                return detail::platform2048_plans;
            case 4096:
                return detail::platform4096_plans;
        }
    }
    // we don't know what happens there
    throw std::invalid_argument("unsupported convolution backend/block pair");
}

inline const PartitionPlan &partition_plan(
        Backend backend, std::int32_t frames, std::int32_t authored_block) {
    if (frames < 0)
        throw std::invalid_argument("negative convolution length");
    const auto plans = partition_table(backend, authored_block);
    const auto retained = backend == Backend::platform_fft
                                  ? std::uint32_t(frames)
                                  : std::min(std::uint32_t(frames),
                                            plans.back().capacity_frames);
    const auto found = std::lower_bound(plans.begin(), plans.end(), retained,
            [](const PartitionPlan &plan, std::uint32_t count) {
                return plan.capacity_frames < count;
            });
    // row 0 if nothing fits, and no clamp first unlike the others
    return found == plans.end() ? plans.front() : *found;
}

// clamp to the last capacity, then the first one that's big enough. negative
// gets rejected
inline const PartitionPlan &ordinary_partition_plan(std::int32_t frames) {
    return partition_plan(Backend::ordinary, frames, 0);
}
} // namespace ekphore::irc::native

#include <bit>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace ekphore::irc::native {

struct WorkVector {
    std::vector<std::int32_t> indices, auxiliary;
    std::vector<std::uint8_t> flags;
    void plain(std::int32_t index) {
        indices.push_back(index);
        flags.push_back(0);
    }
    void flagged(std::int32_t index) {
        indices.push_back(index);
        flags.push_back(1);
    }
    void flagged_with_auxiliary(std::int32_t index, std::int32_t offset) {
        flagged(index);
        auxiliary.push_back(offset);
    }
};
struct FftWork {
    WorkVector forward, inverse;
    std::uint32_t forward_prefix_end{}, inverse_prefix_end{};
};

// the observer can run the butterflies too, the wrapper below doesn't
template <class Observer>
inline FftWork generate_ordinary_fft_work_observed(
        std::uint32_t block, Observer &observer) {
    if (block < 64 || block > (1U << 21) || !std::has_single_bit(block))
        throw std::invalid_argument(
                "FFT work block outside qualified plan dimensions");
    const auto extent = static_cast<std::int32_t>(block * 2);
    const auto last_stage = std::bit_width(block) - 2; // log2(2B)-2
    FftWork result;
    WorkVector deferred_forward, deferred_inverse;
    std::vector<std::int32_t> primary{ extent }, secondary{ extent };
    std::size_t primary_cursor = 0, secondary_cursor = 0,
                inverse_skip_cursor = 0;
    std::int32_t hole_boundary = extent + extent / 2;

    const auto emit_plain = [](WorkVector &output, std::int32_t base,
                                    std::int32_t width) {
        const auto packet = width < 4 ? 1 : 4;
        for (std::int32_t offset = 0; offset < width; offset += packet)
            output.plain(base + offset);
    };
    const auto emit_twiddles = [](WorkVector &output, std::int32_t base,
                                       std::int32_t width) {
        const auto packet = width < 8 ? 1 : 4;
        for (std::int32_t offset = 0; offset < width / 2; offset += packet)
            output.flagged_with_auxiliary(base + width + offset, offset);
    };
    for (unsigned stage = 0; stage <= last_stage; ++stage) {
        if (stage > 1)
            hole_boundary /= std::int32_t(1U << (stage - 1));
        if (stage == last_stage) {
            result.forward_prefix_end =
                    static_cast<std::uint32_t>(result.forward.indices.size());
            result.inverse_prefix_end =
                    static_cast<std::uint32_t>(result.inverse.indices.size());
        }
        const auto width = extent >> (stage + 1), step = 2 * width;
        unsigned hole_depth = 0;
        std::int32_t position = 0;
        while (position < extent) {
            auto last_base = position;
            if (position == hole_boundary) {
                if (hole_depth)
                    last_base +=
                            std::int32_t(2 * ((1U << hole_depth) - 1)) * width;
                auto excluded = position + width;
                do {
                    if (excluded - width == secondary.at(secondary_cursor + 1))
                        ++secondary_cursor;
                    else
                        secondary.push_back(excluded);
                    const bool again = excluded < last_base;
                    excluded += step;
                    if (!again)
                        break;
                } while (true);
                hole_boundary = position * 2;
                ++hole_depth;
                const auto end = position == extent - extent / 4
                                         ? extent
                                         : last_base + step;
                for (auto base = position; base < end; base += step) {
                    if (base == secondary.at(inverse_skip_cursor)) {
                        ++inverse_skip_cursor;
                        continue;
                    }
                    emit_plain(result.inverse, base, width);
                    if (stage != last_stage)
                        emit_twiddles(
                                width < 8 ? deferred_inverse : result.inverse,
                                base, width);
                }
            } else if (position == primary.at(primary_cursor)) {
                ++primary_cursor;
                ++secondary_cursor;
                ++inverse_skip_cursor;
            } else if (position < extent - extent / 4) {
                observer.plain(position, width);
                emit_plain(result.forward, position, width);
                emit_plain(result.inverse, position, width);
                if (stage != last_stage) {
                    observer.twiddle(position, width, stage);
                    emit_twiddles(width < 8 ? deferred_forward : result.forward,
                            position, width);
                    emit_twiddles(width < 8 ? deferred_inverse : result.inverse,
                            position, width);
                }
                primary.push_back(position + width);
                secondary.push_back(position + width);
            }
            position = last_base + step;
        }
        if (!stage) {
            ++inverse_skip_cursor;
            ++primary_cursor;
        }
    }
    const auto append = [](WorkVector &destination, const WorkVector &source) {
        destination.indices.insert(destination.indices.end(),
                source.indices.begin(), source.indices.end());
        destination.flags.insert(destination.flags.end(), source.flags.begin(),
                source.flags.end());
        destination.auxiliary.insert(destination.auxiliary.end(),
                source.auxiliary.begin(), source.auxiliary.end());
    };
    append(result.forward, deferred_forward);
    append(result.inverse, deferred_inverse);

    // the sentinels show up even where there's no forward index
    ++secondary_cursor;
    std::int32_t hole_mark = 6, hole_step = 2;
    unsigned hole_shift = 1;
    for (std::int32_t position = 0; position < extent; position += 2) {
        if (position == hole_mark || position >= extent - extent / 4) {
            auto end = extent;
            if (position != extent - extent / 4) {
                end = position + std::int32_t(1U << hole_shift);
                ++hole_shift;
            }
            for (auto inverse_position = position; inverse_position < end;
                    inverse_position += 2) {
                if (inverse_position == secondary.at(secondary_cursor)) {
                    result.inverse.flagged_with_auxiliary(inverse_position, -1);
                    result.forward.auxiliary.push_back(-1);
                    secondary_cursor = secondary_cursor + 1 == secondary.size()
                                               ? 0
                                               : secondary_cursor + 1;
                } else
                    result.inverse.plain(inverse_position);
            }
            position += hole_step - 2;
            hole_mark *= 2;
            hole_step *= 2;
        } else if (position == primary.at(primary_cursor)) {
            observer.cross_cleanup(position);
            result.forward.flagged_with_auxiliary(position, -1);
            result.inverse.flagged_with_auxiliary(position, -1);
            ++secondary_cursor;
            primary_cursor = primary_cursor + 1 == primary.size()
                                     ? 0
                                     : primary_cursor + 1;
        } else {
            observer.pair_cleanup(position);
            result.forward.plain(position);
            result.inverse.plain(position);
        }
    }
    return result;
}

inline FftWork generate_ordinary_fft_work(std::uint32_t block) {
    struct EmitOnly {
        void plain(std::int32_t, std::int32_t) const {}
        void twiddle(std::int32_t, std::int32_t, unsigned) const {}
        void cross_cleanup(std::int32_t) const {}
        void pair_cleanup(std::int32_t) const {}
    } observer;
    return generate_ordinary_fft_work_observed(block, observer);
}

} // namespace ekphore::irc::native

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ekphore::irc::native {

// reads the generated records, doesn't make them
enum class PrefixOperation {
    real_pair,
    complex_pair,
    twiddle_first,
    twiddle_pair
};
struct PrefixInstruction {
    PrefixOperation operation{};
    std::uint32_t first{}, second{}, twiddle_base{}, twiddle_step{};
    bool operator==(const PrefixInstruction &) const = default;
};

class ForwardPrefix {
  public:
    // prefix_end is NOT the vector length
    [[nodiscard]] static ForwardPrefix prepare(std::uint32_t block_frames,
            std::span<const std::int32_t> indices,
            std::span<const std::uint8_t> flags,
            std::span<const std::int32_t> auxiliary, std::size_t prefix_end,
            std::size_t twiddle_extent);

    [[nodiscard]] std::span<const PrefixInstruction>
    instructions() const noexcept {
        return instruction_records;
    }
    // you can split the work in consecutive ranges, it doesn't change the
    // result. false means a bad range, nothing written
    [[nodiscard]] bool execute(std::span<float> real,
            std::span<float> imaginary, std::span<const float> cosine,
            std::span<const float> sine, std::size_t first_record,
            std::size_t record_count) const noexcept;

    // real only, first B/4. aligned to four so no load crosses the ring end,
    // and the imaginary part doesn't get cleared here
    [[nodiscard]] bool execute_ring(std::span<const float> input_ring,
            std::size_t ring_head, std::span<float> real,
            std::size_t first_record, std::size_t record_count) const noexcept;

  private:
    ForwardPrefix() = default;
    std::uint32_t extent{};
    std::size_t root_extent{};
    bool ring_prefix_compatible{ true };
    std::vector<PrefixInstruction> instruction_records;
};

} // namespace ekphore::irc::native

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ekphore::irc::native {

struct TerminalInstruction {
    std::uint32_t first{}, mirror_one{}, mirror_three{};
    bool rotated{};
    bool operator==(const TerminalInstruction &) const = default;
};

// you give it the vectors and the permutation
class ForwardTerminal {
  public:
    [[nodiscard]] static ForwardTerminal prepare(std::uint32_t block_frames,
            std::span<const std::int32_t> indices,
            std::span<const std::uint8_t> flags,
            std::span<const std::uint32_t> permutation, std::size_t prefix_end,
            std::size_t twiddle_extent);
    [[nodiscard]] std::span<const TerminalInstruction>
    instructions() const noexcept {
        return instruction_records;
    }
    // TWO entries per instruction, an odd bound isn't rounded down so the
    // cursor can end up one past
    [[nodiscard]] std::size_t raw_cursor_after() const noexcept {
        return final_raw_cursor;
    }
    static constexpr std::size_t raw_entries_per_instruction = 2;

    // same as above, ranges in order and false before writing
    [[nodiscard]] bool execute(std::span<float> real,
            std::span<float> imaginary, std::span<const float> cosine,
            std::span<const float> sine, std::size_t first_instruction,
            std::size_t instruction_count) const noexcept;
    // once AFTER execute, real[1] goes to imag[0] and the upper bins stay
    [[nodiscard]] bool pack(
            std::span<float> real, std::span<float> imaginary) const noexcept;

    // same thing but read only working planes, and writes straight into a
    // history slice without normalizing
    [[nodiscard]] bool execute_runtime(std::span<const float> real,
            std::span<const float> imaginary, std::span<const float> cosine,
            std::span<const float> sine, std::span<float> packed_real,
            std::span<float> packed_imaginary, std::size_t first_instruction,
            std::size_t instruction_count) const noexcept;
    // AFTER everything else, and NOT the same as the IR's Nyquist
    [[nodiscard]] bool finish_runtime(std::span<const float> real,
            std::span<float> packed_imaginary) const noexcept;

  private:
    ForwardTerminal() = default;
    std::uint32_t extent{}, twiddle_one{}, twiddle_three{};
    std::size_t root_extent{}, final_raw_cursor{};
    std::vector<TerminalInstruction> instruction_records;
};

} // namespace ekphore::irc::native

namespace ekphore::irc::native {
struct InverseScatterInstruction {
    std::uint32_t first{}, offset1{}, offset3{};
    bool rotated{};
};
// gather is separate, ranges can be split
class InverseTransform {
  public:
    static InverseTransform prepare(std::uint32_t block, const FftWork &work,
            std::span<const std::uint32_t> permutation,
            std::size_t twiddle_extent);
    std::span<const PrefixInstruction> middle() const noexcept {
        return middle_records;
    }
    std::span<const InverseScatterInstruction> terminal() const noexcept {
        return terminal_records;
    }
    // nothing can alias
    bool execute_middle(std::span<float> real, std::span<float> imaginary,
            std::span<const float> cosine, std::span<const float> sine,
            std::size_t first, std::size_t count) const noexcept;
    bool scatter(std::span<const float> real, std::span<const float> imaginary,
            std::span<const float> cosine, std::span<const float> sine,
            std::span<float> ring, std::size_t ring_base, std::size_t first,
            std::size_t count) const noexcept;

  private:
    InverseTransform() = default;
    std::uint32_t block_frames{}, terminal_twiddle{};
    std::size_t root_extent{};
    std::vector<PrefixInstruction> middle_records;
    std::vector<InverseScatterInstruction> terminal_records;
};
} // namespace ekphore::irc::native

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ekphore::irc::native {

struct InverseGatherPacket {
    std::uint32_t destination{}, first_mirror_real{};
    std::array<std::uint32_t, 4> positive{}, mirrored{};
};

// you give it the full/half permutations
class InverseGather {
  public:
    [[nodiscard]] static InverseGather prepare(std::uint32_t block,
            std::span<const std::int32_t> inverse_indices,
            std::span<const std::uint32_t> full_permutation,
            std::span<const std::uint32_t> half_permutation);
    [[nodiscard]] std::span<const InverseGatherPacket>
    packets() const noexcept {
        return gather_packets;
    }
    // real is B+1 with Nyquist at B, NOT in imaginary[0]. imaginary is B with
    // DC at 0. nothing can alias
    [[nodiscard]] bool execute(std::span<const float> packed_real,
            std::span<const float> packed_imaginary, std::span<float> real,
            std::span<float> imaginary, std::size_t first_packet,
            std::size_t packet_count) const noexcept;

  private:
    InverseGather() = default;
    std::uint32_t block_frames{};
    std::vector<InverseGatherPacket> gather_packets;
};
} // namespace ekphore::irc::native

#include <optional>

namespace ekphore::irc::native {
// stereo shares the plans and clock with this
class MonoWorkspace {
  public:
    struct Tier {
        FftWork work;
        std::vector<std::uint32_t> permutation, half_permutation;
        std::vector<float> cosine, sine;
        std::optional<ForwardPrefix> forward;
        std::optional<ForwardTerminal> terminal;
        std::optional<InverseGather> gather;
        std::optional<InverseTransform> inverse;
        std::array<std::vector<float>, 6> accumulation_and_output;
        std::array<std::vector<float>, 2> history, working;
        std::array<std::vector<float>, 2> coefficients;
    };
    static MonoWorkspace prepare(std::int32_t inner_ir_frames);
    // at the inner rate, already converted. padded with zeros, don't pass an
    // empty one
    static MonoWorkspace prepare(std::span<const float> inner_ir);
    // allocates, NEVER from audio
    MonoWorkspace clone() const;
    MonoWorkspace(const MonoWorkspace &) = delete;
    MonoWorkspace &operator=(const MonoWorkspace &) = delete;
    MonoWorkspace(MonoWorkspace &&) noexcept = default;
    MonoWorkspace &operator=(MonoWorkspace &&) noexcept = default;
    [[nodiscard]] bool reset() noexcept;
    const PartitionPlan &plan() const noexcept { return partitioning; }
    std::span<const Tier> tiers() const noexcept { return tier_data; }
    std::span<MonoTierRuntime> runtime() noexcept { return tier_runtime; }
    std::span<float> input_ring() noexcept { return input_storage; }
    std::span<float> output_ring() noexcept { return output_storage; }
    MonoRingIoState &ring_state() noexcept { return io_state; }
    const DirectCoefficients &direct_coefficients() const noexcept {
        return direct;
    }

  private:
    MonoWorkspace() = default;
    static MonoWorkspace prepare_impl(
            std::int32_t frames, std::span<const float> ir);
    PartitionPlan partitioning{};
    std::vector<Tier> tier_data;
    std::vector<MonoTierRuntime> tier_runtime;
    std::vector<float> input_storage, output_storage;
    MonoRingIoState io_state{};
    DirectCoefficients direct{};
};
// 2 for the first tier, the float log/add/square/trunc for the rest
std::uint32_t ordinary_startup_frames(std::uint32_t block, bool first_tier);
} // namespace ekphore::irc::native

#include <utility>

namespace ekphore::irc {
class StagedPairedConvolver;
}

namespace ekphore::irc::native {
// right channel only, the plans and timing live with the left
struct StereoRightRuntime {
    std::array<std::span<float>, 6> accumulation_and_output;
    std::array<std::span<float>, 2> history, working;
    std::array<std::span<const float>, 2> coefficients;
    TailEndpoints endpoints{};
};
class StereoWorkspace {
  public:
    // one length for both, so they have to match
    static StereoWorkspace prepare(
            std::span<const float> left, std::span<const float> right);
    StereoWorkspace clone() const;
    StereoWorkspace(const StereoWorkspace &) = delete;
    StereoWorkspace &operator=(const StereoWorkspace &) = delete;
    StereoWorkspace(StereoWorkspace &&) noexcept = default;
    StereoWorkspace &operator=(StereoWorkspace &&) noexcept = default;
    bool reset() noexcept;
    MonoWorkspace &common() noexcept { return shared_workspace; }
    const MonoWorkspace &common() const noexcept { return shared_workspace; }
    std::span<StereoRightRuntime> right() noexcept { return right_runtime; }
    std::span<const StereoRightRuntime> right() const noexcept {
        return right_runtime;
    }
    std::span<float> right_input_ring() noexcept { return right_input_storage; }
    std::span<float> right_output_ring() noexcept {
        return right_output_storage;
    }
    const DirectCoefficients &right_direct_coefficients() const noexcept {
        return right_direct;
    }
    DirectPackets &right_direct_queue() noexcept { return right_queue; }
    std::span<float, 2> right_boundary() noexcept {
        return right_boundary_values;
    }

  private:
    friend class ::ekphore::irc::StagedPairedConvolver;
    // wrapper only
    std::pair<float, float> process_sample(float left, float right) noexcept;
    explicit StereoWorkspace(MonoWorkspace common)
        : shared_workspace(std::move(common)) {}
    struct RightStorage {
        std::array<std::vector<float>, 6> accumulation_and_output;
        std::array<std::vector<float>, 2> history, working, coefficients;
    };
    MonoWorkspace shared_workspace;
    std::vector<RightStorage> right_storage;
    std::vector<StereoRightRuntime> right_runtime;
    std::vector<float> right_input_storage, right_output_storage;
    DirectCoefficients right_direct{};
    DirectPackets right_queue{};
    std::array<float, 2> right_boundary_values{};
};
} // namespace ekphore::irc::native

#include <memory>
#include <vector>

namespace ekphore::irc {
// move only, so nobody shares the histories by accident
class StagedPairedConvolver {
  public:
    StagedPairedConvolver() = default;
    StagedPairedConvolver(StagedPairedConvolver &&other) noexcept {
        swap(other);
    }
    StagedPairedConvolver &operator=(StagedPairedConvolver &&other) noexcept {
        // this frees the old buffers, on audio swap instead
        if (this != &other) {
            StagedPairedConvolver replacement(std::move(other));
            swap(replacement);
        }
        return *this;
    }
    static StagedPairedConvolver prepare(
            std::vector<std::vector<float>> channels,
            unsigned rate_divisor = 1);
    StagedPairedConvolver clone() const;
    void swap(StagedPairedConvolver &other) noexcept;
    void reset() noexcept;
    std::pair<float, float> process(float left, float right);
    bool configured() const noexcept { return ready; }
    std::size_t impulse_frames() const noexcept {
        return response_frames * convolution_rate.divisor();
    }
    unsigned rate_divisor() const noexcept {
        return convolution_rate.divisor();
    }

  private:
    PairedConvolutionRate convolution_rate;
    bool ready{};
    std::size_t response_frames{};
    std::unique_ptr<native::StereoWorkspace> workspace;
};
} // namespace ekphore::irc

namespace ekphore::irc::native {
class StereoWorkspace;
struct ForwardStageAdmissions {
    std::uint32_t first_work_frames{}, first_input_frames{}, middle_frames{},
            terminal_frames{};
};
// BEFORE the first partition MAC. set input_remaining at the start of the
// callback and reset completed_frames after each ring slice
bool run_forward_stages(const MonoWorkspace::Tier &, MonoTierRuntime &,
        std::span<const float> input_ring,
        ForwardStageAdmissions *admissions = nullptr) noexcept;
// both channels share one cursor update
bool run_stereo_forward_stages(StereoWorkspace &, std::size_t tier,
        ForwardStageAdmissions *admissions = nullptr) noexcept;
} // namespace ekphore::irc::native

namespace ekphore::irc::native {
class StereoWorkspace;
struct InverseStageAdmissions {
    std::uint32_t mac_frames{}, gather_work_frames{}, gather_input_frames{};
    std::uint32_t middle_frames{}, terminal_frames{};
    bool cycle_completed{};
};
// AFTER the forward stages, keep their completed frame count
bool run_inverse_stages(const MonoWorkspace::Tier &, MonoTierRuntime &,
        std::span<float> output_ring,
        InverseStageAdmissions *admissions = nullptr) noexcept;
bool run_stereo_inverse_stages(StereoWorkspace &, std::size_t tier,
        InverseStageAdmissions *admissions = nullptr) noexcept;
} // namespace ekphore::irc::native

#include <cmath>
#include <span>

namespace ekphore::irc::native {

// unlike the fast one, width four uses explicit fmas and the two point cleanup
// stays separate. no contraction
struct IrTransformObserver {
    std::span<float> real, imaginary;
    std::span<const float> cosine, sine;
    std::span<const std::uint32_t> permutation;

    void plain(std::int32_t base, std::int32_t width) {
        for (auto offset = 0; offset < width; ++offset) {
            const auto a = base + offset, b = a + width;
            const auto r0 = real[a], r1 = real[b];
            const auto i0 = imaginary[a], i1 = imaginary[b];
            real[a] = r0 + r1;
            real[b] = r0 - r1;
            imaginary[a] = i0 + i1;
            imaginary[b] = i0 - i1;
        }
    }
    void twiddle(std::int32_t base, std::int32_t width, unsigned stage) {
        for (auto offset = 0; offset < width / 2; ++offset) {
            const auto a = base + width + offset, b = a + width / 2;
            const auto r0 = real[a], r1 = real[b];
            const auto i0 = imaginary[a], i1 = imaginary[b];
            const auto first_real = r0 + i1, first_imaginary = i0 - r1;
            const auto k = static_cast<unsigned>(offset) << stage;
            const auto rotate = [&](auto destination, float r, float i,
                                        unsigned root) {
                if (width < 8 && !root) {
                    real[destination] = r;
                    imaginary[destination] = i;
                } else if (width < 8) {
                    const auto add_real = sine[root] * -i;
                    const auto add_imaginary = i * cosine[root];
                    real[destination] = std::fma(r, cosine[root], add_real);
                    imaginary[destination] =
                            std::fma(r, sine[root], add_imaginary);
                } else {
                    const auto rc = r * cosine[root], is = i * sine[root];
                    const auto rs = r * sine[root], ic = i * cosine[root];
                    real[destination] = rc - is;
                    imaginary[destination] = ic + rs;
                }
            };
            if (base + offset > width)
                rotate(b, r0 - i1, r1 + i0, 3 * k);
            rotate(a, first_real, first_imaginary, k);
        }
    }
    std::uint32_t mirror(std::uint32_t position) const {
        const auto reversed = permutation[position + 1];
        return permutation[reversed ? real.size() - reversed : 0];
    }
    void cross_cleanup(std::int32_t position) {
        const auto r0 = real[position], r1 = real[position + 1];
        const auto i0 = imaginary[position], i1 = imaginary[position + 1];
        real[position] = r0 + i1;
        imaginary[position] = i0 - r1;
        if (position != 2) {
            const auto destination = mirror(position);
            real[destination] = r0 - i1;
            imaginary[destination] = -(i0 + r1);
        }
    }
    void pair_cleanup(std::int32_t position) {
        const auto r0 = real[position], r1 = real[position + 1];
        if (!position) {
            imaginary[0] = r0 - r1;
            real[0] = r0 + r1;
        } else {
            const auto i0 = imaginary[position], i1 = imaginary[position + 1];
            real[position] = r0 + r1;
            imaginary[position] = i0 + i1;
            const auto destination = mirror(position);
            real[destination] = r0 - r1;
            imaginary[destination] = -(i0 - i1);
        }
    }
};

// the first B outputs stay bit reversed, Nyquist in imag[0]
inline FftWork transform_first_ir_partition(std::span<float> real,
        std::span<float> imaginary, std::span<const float> cosine,
        std::span<const float> sine,
        std::span<const std::uint32_t> permutation) {
    const auto extent = real.size();
    if (extent < 128 || extent > (1U << 22) || !std::has_single_bit(extent) ||
            imaginary.size() != extent || cosine.size() < extent ||
            sine.size() < extent || permutation.size() != extent)
        throw std::invalid_argument("invalid first IR transform geometry");
    for (auto value : permutation)
        if (value >= extent)
            throw std::invalid_argument("invalid IR permutation");
    IrTransformObserver observer{ real, imaginary, cosine, sine, permutation };
    auto work = generate_ordinary_fft_work_observed(
            static_cast<std::uint32_t>(extent / 2), observer);
    const auto gain = 1.F / static_cast<float>(extent);
    for (std::size_t source = 2; source < extent; source += 2) {
        imaginary[source / 2] = gain * imaginary[source];
        real[source / 2] = gain * real[source];
    }
    imaginary[0] = gain * imaginary[0];
    real[0] = gain * real[0];
    return work;
}
} // namespace ekphore::irc::native

namespace ekphore::irc::native {
class StereoWorkspace;
// in place is fine, the mirror gets a copy. no SRC or dry/wet here
bool process_mono_workspace(MonoWorkspace &, std::span<const float> input,
        std::span<float> output, std::span<float> mirror = {}) noexcept;
// one clock for both, in place is fine but partial overlap isn't
bool process_stereo_workspace(StereoWorkspace &,
        std::span<const float> left_input, std::span<const float> right_input,
        std::span<float> left_output, std::span<float> right_output) noexcept;
} // namespace ekphore::irc::native

#include <bit>
#include <cmath>
#include <cstdint>
#include <span>
#include <stdexcept>

namespace ekphore::irc::native {
// float sincos, NOT double or a recursive oscillator
inline float twiddle_angle(std::uint32_t index, std::uint32_t extent) noexcept {
    constexpr float two_pi = std::bit_cast<float>(std::uint32_t{ 0x40c90fdb });
    volatile float converted = static_cast<float>(index);
    volatile float denominator = static_cast<float>(extent);
    volatile float product = converted * two_pi;
    volatile float angle = product / denominator;
    return angle;
}
// other libms might not round the same as Apple's
inline void fill_twiddles(
        std::span<float> cosine, std::span<float> negative_sine) {
    const auto n = cosine.size();
    if (n < 128 || n > (1U << 22) || !std::has_single_bit(n) ||
            negative_sine.size() != n)
        throw std::invalid_argument("invalid ordinary twiddle dimensions");
    for (std::uint32_t k = 0; k < n; ++k) {
        const float angle = twiddle_angle(k, static_cast<std::uint32_t>(n));
#if defined(__APPLE__)
        float sine;
        __sincosf(angle, &sine, &cosine[k]);
        negative_sine[k] = -sine;
#else
        cosine[k] = std::cos(angle);
        negative_sine[k] = -std::sin(angle);
#endif
    }
}
} // namespace ekphore::irc::native
