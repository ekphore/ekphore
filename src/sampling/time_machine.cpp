// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "time_machine.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>

#if defined(__clang__)
#pragma clang fp contract(off)
#endif

namespace ekphore::sampling::time_machine {
namespace {
void require_parameters(const State &s) {
    if (s.parameters.analysis_grains &&
            (!s.sample.has_grain_table || s.sample.grain_positions.empty() ||
                    s.sample.displacement_coefficients.size() !=
                            s.sample.grain_positions.size()))
        throw std::invalid_argument(
                "mode3 analyzed grains require the complete owner metadata table");
    if (s.parameters.grain_frames <= 0 || !(s.parameters.engine_rate > 0.F))
        throw std::invalid_argument(
                "mode3 requires positive authored grain length and engine rate");
}
std::int32_t truncate(double x) {
    if (!std::isfinite(x) || x < std::numeric_limits<std::int32_t>::min() ||
            x > std::numeric_limits<std::int32_t>::max())
        throw std::invalid_argument(
                "mode3 position outside native signed32 domain");
    return static_cast<std::int32_t>(x);
}
float control(
        std::span<const float> table, std::size_t offset, float fallback) {
    if (table.empty())
        return fallback;
    if (offset / 32 >= table.size())
        throw std::invalid_argument("mode3 control table too short");
    return table[offset / 32];
}
} // namespace

void reset(State &s, double target) {
    const auto position = truncate(target);
    s.loop_end = s.loop_start = -1;
    s.wrapped = s.finished = s.crossfade_enabled = false;
    s.target = target;
    s.end = s.sample.has_grain_table ? s.sample.metadata_end : 0;
    s.grain_index = -1;
    std::int32_t start = 0;
    if (s.sample.has_grain_table) {
        const auto positions = s.sample.grain_positions;
        if (positions.empty())
            throw std::invalid_argument("empty native grain table");
        auto next =
                std::lower_bound(positions.begin(), positions.end(), position);
        std::size_t index;
        if (next == positions.end())
            index = positions.size() - 1;
        else if (next == positions.begin())
            index = 0;
        else {
            index = static_cast<std::size_t>(next - positions.begin() - 1);
            // a tie keeps the predecessor grain
            if (position - positions[index] > *next - position)
                ++index;
        }
        s.grain_index = static_cast<std::int32_t>(index);
        start = std::min(s.end, positions[index]);
    }
    const float fraction =
            static_cast<float>(target - static_cast<double>(position));
    s.current = s.reset_cursor = { start, fraction };
    s.previous = {};
    s.current_remaining = s.overlap_remaining = 0.F;
    s.first_grain = true;
    // the fade and modulation history stay
}

void copy_coefficients(State &s, double speed_coefficient, float overlap,
        std::int32_t grain_frames, bool analysis_grains) {
    // narrow the coefficient to float without replacing the sample-rate ratio
    s.parameters.speed_coefficient = static_cast<float>(speed_coefficient);
    s.parameters.overlap = overlap;
    if (s.parameters.grain_frames != grain_frames) {
        s.grain_modulation = 0.F;
        s.modulated_grain_frames = grain_frames;
    }
    s.parameters.grain_frames = grain_frames;
    s.parameters.analysis_grains = analysis_grains;
}

bool advance_grain(State &s, float modulation) {
    require_parameters(s);
    auto length = s.modulated_grain_frames;
    if (std::abs(modulation - s.grain_modulation) >= .001F) {
        // keep the exact binary64 bounds for logarithmic grain modulation
        const double low =
                std::bit_cast<double>(std::uint64_t{ 0xbff3437900000000 });
        const double range =
                std::bit_cast<double>(std::uint64_t{ 0x40173c9000000000 });
        const float log_length =
                std::log(static_cast<float>(s.parameters.grain_frames));
        // clang-format off
        const double normalized = std::clamp((static_cast<double>(log_length) -
                low) / range + static_cast<double>(modulation), 0., 1.);
        // clang-format on
        const float milliseconds =
                static_cast<float>(std::exp(low + range * normalized));
        length = std::max<std::int32_t>(16,
                truncate(s.parameters.engine_rate * (milliseconds / 1000.F)));
        s.grain_modulation = modulation;
        s.modulated_grain_frames = length;
    }
    if (length <= 0)
        throw std::invalid_argument("mode3 grain coefficients not initialized");
    s.previous = s.current;
    if (s.loop_start >= 0 && s.loop_end >= 0 &&
            s.loop_end <= truncate(s.target)) {
        s.target = static_cast<double>(s.loop_start);
        s.wrapped = true;
    }
    if (!s.parameters.analysis_grains) {
        // truncate toward zero, not toward minus infinity for negative
        // positions
        s.current.integer = truncate(
                static_cast<double>(length) *
                static_cast<double>(
                        truncate(s.target / static_cast<double>(length))));
        if (s.current.integer >= s.end)
            return false;
        s.current_remaining = static_cast<float>(length);
    } else {
        if (s.target >= static_cast<double>(s.end))
            return false;
        const auto positions = s.sample.grain_positions;
        const auto last = static_cast<std::int32_t>(positions.size() - 1);
        const auto clamped = std::clamp(s.grain_index, 0, last);
        const auto next = std::min(std::max(s.grain_index, -1) + 1, last);
        const auto start = positions[clamped];
        const auto stop = (positions[next] <= s.end || s.end < start)
                                  ? positions[next]
                                  : s.end;
        const double interval = static_cast<double>(stop - start);
        if (s.cached_speed_modulation != s.speed_modulation) {
            s.cached_speed_modulation = s.speed_modulation;
            s.cached_speed_multiplier = static_cast<float>(
                    std::exp2(static_cast<double>(s.speed_modulation) * 2.));
        }
        const float rate = static_cast<float>(
                s.parameters.speed *
                static_cast<double>(s.parameters.speed_coefficient) *
                static_cast<double>(s.cached_speed_multiplier));
        double shift;
        if (rate >= 1.F)
            shift = std::fma(static_cast<double>(rate), -.5, .5);
        else if (rate >= .25F) {
            const float square = rate * rate;
            shift = .5 / static_cast<double>(square) - .5;
        } else
            shift = 8.;
        // multiply the interval first, then fuse with the float displacement
        // coefficient
        const double displacement = interval * shift;
        const auto selected = std::max<std::int32_t>(
                0, truncate(std::fma(displacement,
                           static_cast<double>(
                                   s.sample.displacement_coefficients[clamped]),
                           s.target)));
        std::int32_t probe = truncate(s.target);
        if (selected <= s.end) {
            s.grain_index =
                    grain_index_from_position(s, selected, s.grain_index, true);
            probe = selected;
        }
        s.current.integer =
                std::min(s.end, positions[std::clamp(s.grain_index, 0, last)]);
        if (s.grain_index >= last)
            return false;
        const auto raw_start = positions[std::clamp(s.grain_index, 0, last)];
        if (raw_start > s.end)
            return false;
        auto following = grain_index_from_position(s,
                truncate(static_cast<double>(probe) + length), s.grain_index,
                true);
        if (selected > s.end)
            following = std::min(s.grain_index + 1, last);
        if (following <= s.grain_index)
            ++following;
        following = std::clamp(following, 0, last);
        s.current_remaining = static_cast<float>(
                std::min(s.end, positions[following]) - raw_start);
    }
    const std::int32_t metadata_end =
            s.sample.has_grain_table ? s.sample.metadata_end : 0;
    // overlap still needs the unclipped grain extent
    const float extent = s.current_remaining;
    if (static_cast<float>(metadata_end) <
            s.current_remaining + static_cast<float>(s.current.integer))
        s.current_remaining =
                static_cast<float>(metadata_end - s.current.integer);
    const float old_position = static_cast<float>(s.previous.integer);
    // the metadata end, NOT the cursor, going backward still overlaps the
    // whole grain
    float distance = static_cast<float>(metadata_end - s.previous.integer);
    if (extent + old_position <= static_cast<float>(metadata_end))
        distance = extent;
    s.overlap_remaining = std::max(
            1.F, std::clamp(s.parameters.overlap, 0.F, 1.F) * distance);
    s.fade_phase = 0.F;
    s.fade_increment = 1.F / s.overlap_remaining;
    // advancing a grain changes the integer cursor but keeps its fraction
    return true;
}

std::int32_t grain_index_from_position(const State &s, std::int32_t position,
        std::int32_t hint, bool predecessor) {
    if (!s.sample.has_grain_table || s.sample.grain_positions.empty())
        throw std::invalid_argument("mode3 grain lookup requires metadata");
    const auto positions = s.sample.grain_positions;
    const auto last = static_cast<std::int32_t>(positions.size() - 1);
    if (position >= s.sample.metadata_end)
        return last;
    if (hint < -1 || hint > last)
        throw std::invalid_argument("mode3 grain hint outside owner metadata");
    while (hint > 0 && positions[hint] >= position)
        --hint;
    while (hint < last) {
        const auto next = std::clamp(hint + 1, 0, last);
        const auto delta =
                static_cast<std::int64_t>(positions[next]) - position;
        if (delta >= 0) {
            const auto current = positions[std::clamp(hint, 0, last)];
            if (predecessor ||
                    static_cast<std::int64_t>(position) - current <= delta)
                return hint;
            return next < last ? next : hint;
        }
        ++hint;
    }
    // with a loop the last lookup goes back to 0
    return s.loop_start >= 0 && s.loop_end >= 0 ? 0 : last;
}

float crossfade_weight(float phase) {
    float x = phase + phase;
    if (x <= 1.F) {
        x = std::max(0.F, x);
        const float half = x * .5F;
        return x * (x * half); // keep the multiplication grouping
    }
    x = std::max(0.F, 2.F - x);
    const float half = x * -.5F;
    return std::fma(x, x * half, 1.F);
}

void advance_time(State &s, std::size_t frames) {
    if (s.cached_speed_modulation != s.speed_modulation) {
        s.cached_speed_modulation = s.speed_modulation;
        s.cached_speed_multiplier = static_cast<float>(
                std::exp2(static_cast<double>(s.speed_modulation) * 2.));
    }
    const float rate = static_cast<float>(
            s.parameters.speed *
            static_cast<double>(s.parameters.speed_coefficient) *
            static_cast<double>(s.cached_speed_multiplier));
    const float advance = static_cast<float>(frames) * rate;
    s.target += static_cast<double>(advance);
}

std::size_t render(State &s, FillMode mode, std::span<float *const> output,
        std::span<float *const> scratch, std::size_t frames,
        const Controls &controls, Fill fill, void *context) {
    require_parameters(s);
    if (!fill || output.empty() || output.size() > 16 ||
            output.size() != scratch.size())
        throw std::invalid_argument(
                "mode3 needs1..16 channels and matching scratch/fill");
    if (mode == FillMode::modulated && controls.pitch.size() < frames)
        throw std::invalid_argument("mode3 per-frame pitch table too short");
    if (!std::isfinite(controls.pitch_ratio) || controls.pitch_ratio <= 0.F)
        throw std::invalid_argument(
                "mode3 forward pitch ratio must be positive");
    std::array<float *, 16> out{}, old{};
    std::size_t consumed = 0, speed_offset = 0, grain_offset = 0,
                overlap_offset = 0;
    float grain = 0.F;
    s.speed_modulation = 0.F; // reset on every call
    while (consumed < frames) {
        s.speed_modulation = std::clamp(
                control(controls.speed, speed_offset, s.speed_modulation),
                -1.5F, 1.5F);
        for (std::size_t c = 0; c < output.size(); ++c) {
            if (!output[c] || !scratch[c])
                throw std::invalid_argument("null mode3 PCM buffer");
            out[c] = output[c] + consumed;
            old[c] = scratch[c];
        }
        const auto pitch = mode == FillMode::modulated
                                   ? controls.pitch.subspan(consumed)
                                   : std::span<const float>{};
        const auto before = s.current.integer;
        const auto produced = fill(context, mode, s.current,
                before + truncate(s.current_remaining),
                std::span<float *const>(out.data(), output.size()),
                frames - consumed, controls.pitch_ratio, pitch);
        if (produced > frames - consumed)
            throw std::logic_error("invalid mode3 fill frame count");
        if (s.overlap_remaining >= 1.F) {
            const auto previous_start = s.previous.integer;
            const auto overlap_frames = fill(context, mode, s.previous,
                    previous_start + truncate(s.overlap_remaining),
                    std::span<float *const>(old.data(), output.size()),
                    produced, controls.pitch_ratio, pitch);
            if (overlap_frames > produced)
                throw std::logic_error("invalid mode3 overlap frame count");
            s.overlap_remaining -=
                    static_cast<float>(s.previous.integer - previous_start);
            if (s.crossfade_enabled) {
                for (std::size_t i = 0; i < overlap_frames; ++i) {
                    const float weight = crossfade_weight(s.fade_phase);
                    for (std::size_t c = 0; c < output.size(); ++c) {
                        const float current = weight * out[c][i];
                        const float previous = (1.F - weight) * old[c][i];
                        out[c][i] = std::fma(
                                previous, s.first_grain ? 0.F : 1.F, current);
                    }
                    if (!s.parameters.pitch_scaled_crossfade ||
                            mode == FillMode::fast)
                        s.fade_phase += s.fade_increment;
                    else if (mode == FillMode::resampled)
                        s.fade_phase = std::fma(controls.pitch_ratio,
                                s.fade_increment, s.fade_phase);
                    else {
                        const float increment = pitch[i] * s.fade_increment;
                        s.fade_phase = std::fma(
                                increment, controls.pitch_ratio, s.fade_phase);
                    }
                }
            }
        }
        s.current_remaining -= static_cast<float>(s.current.integer - before);
        advance_time(s, produced);
        consumed += produced;
        if (!controls.grain.empty()) {
            grain = control(controls.grain, grain_offset, 0.F);
            grain_offset += produced;
        }
        // per-frame pitch mode doesn't consume the overlap control table
        if (mode != FillMode::modulated && !controls.overlap.empty()) {
            s.parameters.overlap = control(
                    controls.overlap, overlap_offset, s.parameters.overlap);
            overlap_offset += produced;
        }
        if (consumed == frames)
            break;
        if (produced) {
            s.first_grain = false;
            s.crossfade_enabled = true;
        }
        speed_offset += produced;
        if (!advance_grain(s, grain)) {
            s.finished = true;
            break;
        }
        // the first grain can be empty, but if the next one is too we're stuck
        if (s.current_remaining <= 0.F)
            throw std::invalid_argument(
                    "mode3 grain table/end metadata inconsistent");
    }
    return consumed;
}
} // namespace ekphore::sampling::time_machine
