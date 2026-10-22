// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "wavetable.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>
#include <numbers>
#include <stdexcept>

namespace ekphore::sampling::wavetable {
namespace {

constexpr std::array<MipLayout, mip_count> layout{
    { { 4096, 20, 2048, 0.0023640599101781845f },
            { 4096, 4156, 1625, 0.0029850699938833714f },
            { 4096, 8292, 1290, 0.003759389976039529f },
            { 2048, 12428, 1024, 0.004716979805380106f },
            { 2048, 14516, 812, 0.005988019984215498f },
            { 2048, 16604, 645, 0.0075187901966273785f },
            { 1024, 18692, 512, 0.009433959610760212f },
            { 1024, 19756, 406, 0.011904760263860226f },
            { 1024, 20820, 322, 0.015151510015130043f },
            { 512, 21884, 256, 0.018867919221520424f },
            { 512, 22436, 203, 0.02380952052772045f },
            { 512, 22988, 161, 0.030303029343485832f },
            { 256, 23540, 128, 0.03703702986240387f },
            { 256, 23836, 101, 0.0476190410554409f },
            { 256, 24132, 80, 0.0625f },
            { 128, 24428, 64, 0.0714285671710968f },
            { 128, 24596, 50, 0.10000000149011612f },
            { 128, 24764, 40, 0.125f }, { 64, 24932, 32, 0.14285710453987122f },
            { 64, 25036, 25, 0.20000000298023224f }, { 64, 25140, 20, 0.25f },
            { 32, 25244, 16, 0.25f }, { 32, 25316, 12, 0.5f },
            { 32, 25388, 10, 0.5f }, { 16, 25460, 8, 0.5f },
            { 16, 25516, 6, 0.5f }, { 16, 25572, 4, 1.0f },
            { 8, 25628, 3, 1.0f }, { 8, 25676, 2, 0.0f } }
};
static_assert(
        layout.back().start + layout.back().size + 20 == packed_frame_size);

// our own FFT up to 4096, unnormalized like R2HC/HC2R. it won't round exactly
// like FFTW
void fft(std::span<std::complex<double>> values, bool inverse) {
    const auto n = values.size();
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        auto bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
            std::swap(values[i], values[j]);
    }
    for (std::size_t length = 2; length <= n; length <<= 1) {
        const double angle = (inverse ? 2.0 : -2.0) * std::numbers::pi /
                             static_cast<double>(length);
        const std::complex<double> step{ std::cos(angle), std::sin(angle) };
        for (std::size_t base = 0; base < n; base += length) {
            std::complex<double> twiddle{ 1, 0 };
            for (std::size_t j = 0; j < length / 2; ++j) {
                const auto even = values[base + j];
                const auto odd = values[base + j + length / 2] * twiddle;
                values[base + j] = even + odd;
                values[base + j + length / 2] = even - odd;
                twiddle *= step;
            }
        }
    }
}

float cubic(
        std::span<const float> table, std::size_t offset, float t) noexcept {
    // round coefficient operations separately, then fuse the Horner evaluation
    const float previous = table[offset - 1], current = table[offset];
    const float next = table[offset + 1], after = table[offset + 2];
    const float a = (next - previous) * 0.5f;
    const float b = (after - current) * 0.5f;
    const float c = current - next;
    const float d = c + a, e = d + b, h = c + e;
    return std::fma(t, std::fma(t, std::fma(t, h, -(d + h)), a), current);
}

float inactive_mix_factor() noexcept {
    // the zero-angle polynomial is close to unity, but replacing it with 1
    // would change the output gain
    const float square = 0.25f, fourth = square * square,
                sixth = square * fourth;
    double y = std::fma(static_cast<double>(sixth), -0x1.14a86b3be8e6ep-1,
            static_cast<double>(fourth) * 0x1.449061abaa342p+1);
    y = std::fma(static_cast<double>(square), -0x1.4aa825e78317bp+2, y);
    y += 0x1.921fb54442d18p+1;
    return 0.5f * static_cast<float>(y);
}

void select_mip(State &s) noexcept {
    if (s.phase_increment == 0) {
        s.low_mip = s.high_mip = 0;
        s.mip_blend = 0;
        return;
    }
    const float period = 1.0f / std::abs(s.phase_increment);
    if (period <= 2) {
        s.low_mip = s.high_mip = 28;
        s.mip_blend = 0;
    } else if (period >= 2047) {
        s.low_mip = s.high_mip = 0;
        s.mip_blend = 0;
    } else {
        if (!(period <= layout[s.low_mip].cutoff &&
                    layout[s.high_mip].cutoff < period)) {
            s.high_mip = 1;
            while (layout[s.high_mip].cutoff >= period)
                ++s.high_mip;
            s.low_mip = s.high_mip - 1;
        }
        s.mip_blend = (layout[s.low_mip].cutoff - period) *
                      layout[s.low_mip].inverse_range;
    }
}

float interpolate(std::span<const float> first, std::span<const float> second,
        float frame_fraction, const State &s) noexcept {
    const auto at_frame = [&](std::span<const float> table) {
        const auto at_mip = [&](std::uint32_t level) {
            const float size = static_cast<float>(layout[level].size);
            const float coordinate = s.phase * size;
            const auto offset = static_cast<std::size_t>(coordinate);
            const float fraction =
                    std::fma(s.phase, size, -static_cast<float>(offset));
            return cubic(table, layout[level].start + offset, fraction);
        };
        const float a = at_mip(s.low_mip), b = at_mip(s.high_mip);
        return std::fma(b - a, s.mip_blend, a);
    };
    const float a = at_frame(first), b = at_frame(second);
    return std::fma(b - a, frame_fraction, a);
}

float sample(const PreparedData &data, State &s, float position,
        float frequency) noexcept {
    select_mip(s); // select with the previous sample's increment
    const float coordinate =
            position * static_cast<float>(data.frame_count() - 1);
    const auto a = static_cast<std::size_t>(coordinate);
    const auto b = std::min(a + 1, data.frame_count() - 1);
    const float fraction = coordinate - static_cast<float>(a);
    const float p = interpolate(data.primary(a), data.primary(b), fraction, s);
    // inharmonic off only reads the primary tables
    const float value = p;
    s.phase += s.phase_increment; // advance with the old step before setting
                                  // frequency
    if (s.phase > 1 || s.phase < 0)
        s.phase =
                std::fmod(s.phase >= 0 ? s.phase : 1 - std::abs(s.phase), 1.0f);
    s.phase_increment = frequency * s.reciprocal_rate;
    s.previous_outputs = { value, s.previous_outputs[0] };
    return value * inactive_mix_factor();
}

bool finite(std::span<const float> values) noexcept {
    return std::all_of(values.begin(), values.end(),
            [](float x) { return std::isfinite(x); });
}
bool unit(float x) noexcept { return std::isfinite(x) && x >= 0 && x <= 1; }
bool inactive(const std::array<float, 16> &values) noexcept {
    for (std::size_t i = 0; i < values.size(); ++i)
        if (i != position && i != frequency && i != warp1 && i != warp2 &&
                i != Lane::inharmonic && values[i] != 0)
            return false;
    return true;
}
} // namespace

std::span<const MipLayout, mip_count> mip_layout() noexcept { return layout; }
std::span<const float> PreparedData::primary(std::size_t frame) const noexcept {
    if (frame >= frame_count())
        return {};
    return std::span<const float>(primary_data)
            .subspan(frame * packed_frame_size, packed_frame_size);
}
std::span<const float> PreparedData::auxiliary(
        std::size_t frame) const noexcept {
    if (frame >= frame_count())
        return {};
    return std::span<const float>(auxiliary_data)
            .subspan(frame * packed_frame_size, packed_frame_size);
}

PreparedData prepare(std::span<const float> pcm, PrepareLimits limits) {
    if (pcm.empty())
        throw std::invalid_argument("empty wavetable PCM");
    // consume consecutive full frames up to the cap, or one padded short frame
    const auto count = std::min(maximum_table_frames,
            std::max(std::size_t{ 1 }, pcm.size() / source_frame_size));
    const auto bytes = count * (2 * packed_frame_size + 1) * sizeof(float);
    if (count > limits.max_table_frames || bytes > limits.max_prepared_bytes)
        throw std::length_error("wavetable preparation budget exceeded");
    const auto consumed = std::min(pcm.size(), count * source_frame_size);
    if (!finite(pcm.first(consumed)))
        throw std::invalid_argument("nonfinite wavetable PCM");

    PreparedData result;
    result.primary_data.resize(count * packed_frame_size);
    result.auxiliary_data.resize(count * packed_frame_size);
    result.dc.resize(count);
    std::array<std::complex<double>, 4096> work{};
    std::array<float, source_frame_size> hc{};
    constexpr float scale = 1.0f / 4096.0f;
    for (std::size_t frame = 0; frame < count; ++frame) {
        std::fill(work.begin(), work.end(), std::complex<double>{});
        const auto length = std::min(
                source_frame_size, pcm.size() - frame * source_frame_size);
        for (std::size_t i = 0; i < length; ++i)
            work[i] = pcm[frame * source_frame_size + i];
        fft(std::span(work).first(source_frame_size), false);
        for (std::size_t k = 0; k <= 1024; ++k)
            hc[k] = static_cast<float>(work[k].real());
        for (std::size_t k = 1; k < 1024; ++k)
            hc[2048 - k] = static_cast<float>(work[k].imag());
        result.dc[frame] = hc[0] * scale;
        hc[1024] = 0;
        for (int branch = 0; branch < 2; ++branch) {
            if (branch) {
                hc[0] = 0;
                for (std::size_t k = 1; k < 1024; ++k) {
                    const float real = hc[k];
                    hc[k] = hc[2048 - k];
                    hc[2048 - k] = -real;
                }
            }
            auto &packed = branch ? result.auxiliary_data : result.primary_data;
            for (const auto &mip : layout) {
                const auto n = mip.size;
                const auto bins = static_cast<std::size_t>(mip.cutoff * 0.5f);
                // the bin ranges are asymmetric: real [0,l), imaginary [1,l]
                std::fill(work.begin(), work.end(), std::complex<double>{});
                // build the bins directly, saves another 4096 float buffer
                for (std::size_t k = 0; k < bins; ++k)
                    work[k].real(hc[k]);
                for (std::size_t k = 1; k <= bins; ++k)
                    work[k].imag(hc[2048 - k]);
                for (std::size_t k = 1; k < n / 2; ++k)
                    work[n - k] = std::conj(work[k]);
                fft(std::span(work).first(n), true);
                for (std::size_t i = 0; i < n; ++i) {
                    // narrow the inverse output to float before scaling
                    const float value =
                            static_cast<float>(work[i].real()) * scale;
                    if (!std::isfinite(value))
                        throw std::invalid_argument(
                                "wavetable transform overflow");
                    packed[frame * packed_frame_size + mip.start + i] = value;
                }
                for (int i = -20; i < static_cast<int>(n) + 20; ++i) {
                    if (i >= 0 && i < static_cast<int>(n))
                        continue;
                    const auto wrapped =
                            (i % static_cast<int>(n) + static_cast<int>(n)) %
                            static_cast<int>(n);
                    const auto base = frame * packed_frame_size + mip.start;
                    packed[static_cast<std::size_t>(
                            static_cast<std::ptrdiff_t>(base) + i)] =
                            packed[base + static_cast<std::size_t>(wrapped)];
                }
            }
        }
    }
    return result;
}

bool initialize(State &state, float sample_rate) noexcept {
    if (!std::isfinite(sample_rate) || sample_rate <= 0)
        return false;
    const float milliseconds = sample_rate * 0.001f;
    const float blocks = milliseconds * 0.03125f;
    const float rounded = blocks + 0.5f;
    // the int32 conversion and later shift must both stay representable
    if (!std::isfinite(rounded) || rounded >= 67108864.0f)
        return false;
    const auto count =
            std::max(std::int32_t{ 1 }, static_cast<std::int32_t>(rounded));
    state.current.fill(0);
    state.increments.fill(0);
    state.targets.fill(0);
    state.dirty = false;
    state.remaining_rampblocks = 0;
    state.rampblocks = static_cast<std::uint32_t>(count);
    state.smoothing_step = 1.0f / static_cast<float>(count << 5);
    state.reciprocal_rate =
            static_cast<float>(1.0 / static_cast<double>(sample_rate));
    return true;
}

bool reset_source(State &state, const std::array<float, 7> &authored,
        float realized_phase) noexcept {
    if (!finite(authored) || !std::isfinite(realized_phase))
        return false;
    for (std::size_t i = 0; i < authored.size(); ++i) {
        state.current[i] = state.targets[i] = authored[i];
        state.increments[i] = 0;
    }
    state.phase = realized_phase;
    state.secondary_phase = 0.25f;
    state.instant_position = true;
    return true;
}

bool set_frequency(State &state, float hz) noexcept {
    if (!std::isfinite(hz) || hz < 0)
        return false;
    if (state.targets[frequency] == 0) {
        state.current[frequency] = hz;
        state.increments[frequency] = 0;
    } else {
        state.dirty = true;
    }
    state.targets[frequency] = hz;
    return true;
}

float voice_frequency(std::int32_t note, bool key_tracking, float event_tune,
        float master_tune, float group_tune, float program_tune,
        float zone_tune) noexcept {
    // clang-format off
    const float base = key_tracking ?
            static_cast<float>(std::exp2((static_cast<double>(note) - 69.0) /
            12.0)) * 440.0f : 440.0f;
    // clang-format on
    float ratio = event_tune * master_tune;
    ratio = ratio * group_tune;
    ratio = ratio * program_tune;
    ratio = ratio * zone_tune;
    return ratio * base;
}

RenderStatus render(const PreparedData &data, State &state,
        const Variant &variant, const Controls &controls, std::size_t frames,
        std::span<const std::span<float>> outputs,
        std::size_t first_frame) noexcept {
    if (variant.quality != 1 || variant.inharmonic_enabled ||
            variant.warp_modes != std::array<std::uint32_t, 2>{} ||
            variant.modulation_selector != 0 || !controls.warp2.empty() ||
            !controls.inharmonic.empty() ||
            !controls.modulation_amount.empty() ||
            !controls.modulation_frequency.empty())
        return RenderStatus::unsupported_variant;
    if (!inactive(state.current) || !inactive(state.targets) ||
            !inactive(state.increments))
        return RenderStatus::unsupported_variant;
    if (first_frame > maximum_render_frames ||
            frames > maximum_render_frames - first_frame)
        return RenderStatus::frame_limit;
    if (data.frame_count() == 0 || data.frame_count() > maximum_table_frames)
        return RenderStatus::invalid_data;
    if (outputs.empty() ||
            std::any_of(outputs.begin(), outputs.end(),
                    [frames](auto out) { return out.size() < frames; }))
        return RenderStatus::invalid_output;
    const auto end_frame = first_frame + frames;
    const auto ticks = (end_frame + 31) / 32;
    const auto plane_valid = [ticks](auto plane) {
        return plane.empty() ||
               (plane.size() >= ticks && finite(plane.first(ticks)));
    };
    if (!plane_valid(controls.position) || !plane_valid(controls.warp1) ||
            (!controls.pitch.empty() &&
                    (controls.pitch.size() < end_frame ||
                            !finite(controls.pitch.first(end_frame)))))
        return RenderStatus::invalid_controls;
    if (!unit(state.phase) || !unit(state.phase_increment) ||
            !unit(state.mip_blend) || state.low_mip >= mip_count ||
            state.high_mip >= mip_count ||
            !std::isfinite(state.reciprocal_rate) ||
            state.reciprocal_rate <= 0 ||
            !std::isfinite(state.secondary_phase) ||
            !unit(state.current[inharmonic]) ||
            !unit(state.targets[inharmonic]) ||
            !unit(state.current[position]) || !unit(state.targets[position]) ||
            !finite(state.current) || !finite(state.targets) ||
            !finite(state.increments) || !finite(state.previous_outputs) ||
            state.rampblocks == 0 || state.rampblocks > 1000000 ||
            !std::isfinite(state.smoothing_step) || state.smoothing_step <= 0 ||
            state.smoothing_step > 1.0f / 32)
        return RenderStatus::invalid_state;
    State s = state;
    for (std::size_t i = first_frame; i < end_frame; ++i) {
        const bool boundary = i % 32 == 0;
        if (boundary) {
            if (!controls.warp1.empty()) {
                s.targets[warp1] =
                        std::clamp(controls.warp1[i / 32], 0.0f, 1.0f);
                s.dirty = true;
            }
            if (!controls.position.empty()) {
                s.targets[position] =
                        std::clamp(controls.position[i / 32], 0.0f, 1.0f);
                if (s.instant_position) {
                    s.current[position] = s.targets[position];
                    s.increments[position] = 0;
                } else
                    s.dirty = true;
            }
        }
        const float local_position = s.current[position],
                    local_frequency = s.current[frequency];
        if (boundary) {
            if (s.dirty) {
                s.dirty = false;
                bool any_delta = false;
                for (std::size_t lane = 0; lane < 16; ++lane) {
                    float delta = (s.targets[lane] - s.current[lane]) *
                                  s.smoothing_step;
                    if (static_cast<double>(delta * delta) < 1.0e-15) {
                        delta = 0;
                        s.current[lane] = s.targets[lane];
                    } else
                        any_delta = true;
                    s.increments[lane] = delta;
                }
                if (any_delta)
                    s.remaining_rampblocks = s.rampblocks;
            } else {
                // wrap the int32 countdown without C++ signed overflow
                s.remaining_rampblocks =
                        s.remaining_rampblocks ==
                                        std::numeric_limits<std::int32_t>::min()
                                ? std::numeric_limits<std::int32_t>::max()
                                : s.remaining_rampblocks - 1;
                if (s.remaining_rampblocks == 0) {
                    s.current = s.targets;
                    s.increments.fill(0);
                }
            }
        }
        for (std::size_t lane = 0; lane < 16; ++lane)
            s.current[lane] += s.increments[lane];
        const float frequency =
                local_frequency *
                (controls.pitch.empty() ? 1.0f : controls.pitch[i]);
        if (!unit(local_position) || !std::isfinite(frequency) ||
                frequency < 0 || !unit(frequency * s.reciprocal_rate) ||
                !unit(s.current[position]) || !finite(s.current))
            return RenderStatus::invalid_state;
        const float value = sample(data, s, local_position, frequency);
        if (!std::isfinite(value))
            return RenderStatus::invalid_state;
        for (auto output : outputs)
            output[i - first_frame] = value;
        s.instant_position = false;
    }
    state = s;
    return RenderStatus::ok;
}

} // namespace ekphore::sampling::wavetable
