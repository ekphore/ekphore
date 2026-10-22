// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "irc.h"

// preparation needs separate float operations even if callers allow contraction
#if defined(__clang__)
#pragma clang fp contract(off)
#elif defined(__GNUC__)
#pragma GCC push_options
#pragma GCC optimize("fp-contract=off")
#endif
#include "irc_pv.h"

#include "../../sampling/audio_file.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#if defined(__APPLE__)
#include <Accelerate/Accelerate.h>
#endif

namespace ekphore::irc {

std::optional<RatePlan> rate_plan(const BranchParameters &p, std::uint32_t rate,
        std::uint32_t decoded_rate) noexcept {
    if (!rate || !decoded_rate)
        return std::nullopt;
    RatePlan plan{ decoded_rate, rate, 1 };
    if (p.sample_rate_decimation && *p.sample_rate_decimation >= 0.F) {
        const float d = *p.sample_rate_decimation;
        if (d != 1.F && d != 2.F)
            return std::nullopt;
        if (d == 2.F && (rate & 1))
            return std::nullopt;
        if (!p.preserve_ir_length)
            return std::nullopt;
        plan.divisor = unsigned(d);
        plan.processing_rate = rate / plan.divisor;
        if (!*p.preserve_ir_length) {
            volatile float divided = float(decoded_rate) / d;
            volatile float rounded = divided + .5F;
            if (!(rounded >= 1.F) || rounded >= 0x1p32F)
                return std::nullopt;
            plan.import_rate = std::uint32_t(rounded);
        }
    } else if ((p.sample_rate_decimation &&
                       !std::isfinite(*p.sample_rate_decimation)) ||
               decoded_rate < rate)
        return std::nullopt;
    if (p.processing_sample_rate &&
            p.processing_sample_rate != plan.processing_rate)
        return std::nullopt;
    return plan;
}

bool branch_preparation_supported(const BranchParameters &p, std::uint32_t rate,
        std::size_t channels, std::uint32_t decoded_rate) noexcept {
#if defined(__APPLE__)
    const auto valid = [](float x) {
        return std::isfinite(x) && x >= .5F && x <= 1.5F;
    };
    const auto frequency = [](float f) { return std::isfinite(f) && f >= 0.F; };
    return rate && (channels == 1 || channels == 2) &&
           // import at the engine rate first, a source at or above it keeps
           // that rate with automatic decimation
           rate_plan(p, rate, decoded_rate ? decoded_rate : rate).has_value() &&
           valid(p.early_stretch) && valid(p.late_stretch) &&
           valid(p.early_pitch) && valid(p.late_pitch) &&
           std::isfinite(p.crosspoint) && p.crosspoint <= 1 &&
           frequency(p.early_low_pass_hz) && frequency(p.early_high_pass_hz) &&
           frequency(p.late_low_pass_hz) && frequency(p.late_high_pass_hz);
#else
    (void)p;
    (void)rate;
    (void)channels;
    (void)decoded_rate;
    return false;
#endif
}

std::vector<std::vector<float>> prepare_response(
        std::span<const std::vector<float>> source, std::uint32_t decoded_rate,
        const FilePreparationRequest &request) {
    if (!decoded_rate || source.empty() || source.front().empty() ||
            std::ranges::any_of(source, [&](const auto &c) {
                return c.size() != source.front().size();
            }))
        throw std::invalid_argument("invalid IRC source planes");
    const auto rate = request.engine_sample_rate ? request.engine_sample_rate
                                                 : decoded_rate;
    const auto plan =
            request.branches && branch_preparation_supported(*request.branches,
                                        rate, source.size(), decoded_rate)
                    ? rate_plan(*request.branches, rate, decoded_rate)
                    : std::nullopt;
    const auto processing_rate = plan ? plan->processing_rate : rate;
    // import at Fs BEFORE anything else
    std::vector<std::vector<float>> channels(source.begin(), source.end());
    for (auto &channel : channels)
        channel = import_channel(channel,
                float(plan ? plan->import_rate : decoded_rate), float(rate));
    const bool automatic =
            request.branches && request.branches->crosspoint < 0.F;
    std::optional<std::size_t> analyzed_crosspoint;
    if (automatic && branch_preparation_supported(*request.branches, rate,
                             channels.size(), decoded_rate)) {
        // BEFORE reversing
        analyzed_crosspoint = automatic_crosspoint(channels, float(rate));
        if (!analyzed_crosspoint && plan && plan->divisor != 1)
            throw std::invalid_argument(
                    "unsupported reduced-rate IRC automatic split geometry");
        if (!analyzed_crosspoint)
            std::clog << "IRC automatic split unsupported geometry; retained "
                         "legacy path\n";
    }
    if (request.reverse)
        for (auto &channel : channels)
            std::ranges::reverse(channel);
#if defined(__APPLE__)
    if (request.branches &&
            branch_preparation_supported(
                    *request.branches, rate, channels.size(), decoded_rate) &&
            (!automatic || analyzed_crosspoint)) {
        const auto &p = *request.branches;
        const auto frames = channels.front().size();
        if (frames >= std::size_t(INT32_MAX / 4))
            throw std::invalid_argument("IRC preparation extent overflow");
        // 16384 with the default block/cap
        constexpr std::size_t quantum = 16384;
        const auto initial = (frames / quantum + 1) * quantum;
        volatile float scaled = float(initial) * 1.5F;
        volatile float units = scaled / float(quantum) + 1.F;
        // ceil, NOT a cast
        const auto capacity =
                quantum * std::size_t(std::int32_t(std::ceil(float(units))));
        // length is N the first time, and four separate float ops here
        volatile float position = float(frames) * p.crosspoint;
        volatile float seconds = position / float(rate);
        volatile float sample_position = float(rate) * seconds;
        volatile float rounded = sample_position + .5F;
        auto crosspoint = automatic ? *analyzed_crosspoint
                                    : std::size_t(std::int32_t(rounded));
        if (request.reverse)
            crosspoint = frames - crosspoint;
        pv::FrameStreamBackend backend;
        pv::ElastiqueStream stream(
                float(rate), unsigned(channels.size()), 256, backend);
        std::vector<std::span<const float>> views;
        for (const auto &channel : channels)
            views.emplace_back(channel);
        const auto stretch = [&](float q, float pitch) {
            volatile float ratio = float(rate) / float(processing_rate);
            volatile float stretch_ratio = q / ratio;
            volatile float pitch_ratio = ratio * pitch;
            q = stretch_ratio;
            pitch = pitch_ratio;
            if (q == 1.F && pitch == 1.F) {
                auto result = channels;
                for (auto &lane : result)
                    lane.resize(capacity);
                return result;
            }
            return stream.apply_offline(views, q, pitch, capacity);
        };
        auto early = stretch(p.early_stretch, p.early_pitch);
        auto late = stretch(p.late_stretch, p.late_pitch);
        // padded to trunc(float(N)*1.5), LP then HP, per channel
        volatile float padded = float(frames) * 1.5F;
        const auto filter_frames = std::size_t(std::int32_t(padded));
        apply_ir_filter(early, filter_frames,
                ir_filter_coefficients(IrFilterKind::low_pass,
                        p.early_low_pass_hz, float(rate)));
        apply_ir_filter(early, filter_frames,
                ir_filter_coefficients(IrFilterKind::high_pass,
                        p.early_high_pass_hz, float(rate)));
        apply_ir_filter(late, filter_frames,
                ir_filter_coefficients(IrFilterKind::low_pass,
                        p.late_low_pass_hz, float(rate)));
        apply_ir_filter(late, filter_frames,
                ir_filter_coefficients(IrFilterKind::high_pass,
                        p.late_high_pass_hz, float(rate)));
        channels = merge_early_late(early, late,
                { float(rate), float(processing_rate), p.early_stretch,
                        p.late_stretch, .05F, frames, crosspoint });
        apply_envelope(channels, request.envelope);
        // from the partition IFFTs, NOT the merged response
        constexpr unsigned block = 1024;
        for (auto &lane : channels) {
            std::size_t pos = 0;
            unsigned size = 2 * block;
            for (unsigned part = 0; pos < lane.size(); ++part) {
                std::vector<float> work(size);
                const auto count =
                        std::min<std::size_t>(size / 2, lane.size() - pos);
                std::copy_n(lane.data() + pos, count, work.data());
                pv::RealFft fft(size);
                fft.forward(work);
                fft.inverse(work);
                const float scale = 1.F / float(size);
                vDSP_vsmul(work.data(), 1, &scale, work.data(), 1, size);
                std::copy_n(work.data(), count, lane.data() + pos);
                pos += size / 2;
                if ((part & 1) && size < 2 * std::min(8 * block, 16384U))
                    size *= 2;
            }
        }
    } else
#endif
        apply_envelope(channels, request.envelope);
    return prepare_low_latency_ir(channels, request.convolution_block_size,
            request.auto_gain,
            pre_delay_frames(request.pre_delay_ms, processing_rate));
}

PreparedFile prepare_file(const FilePreparationRequest &request) {
    if (request.resolved_path.empty() || request.requested_path.empty())
        throw std::invalid_argument("empty IR filename");
    DecodedSample decoded;
    std::error_code error;
    if (std::filesystem::exists(request.resolved_path, error) || error ||
            !request.resource_reader) {
        decoded = sampling::decode_sample_file(request.resolved_path);
    } else {
        const auto bytes = request.resource_reader(request.requested_path);
        if (!bytes)
            throw std::runtime_error("IR resource was not found");
        decoded = sampling::decode_sample_bytes(*bytes);
    }
    if (!decoded.sample_rate || decoded.channels.empty() ||
            std::ranges::all_of(decoded.channels,
                    [](const auto &channel) { return channel.empty(); }))
        throw std::runtime_error("IR has no decoded audio");
    PreparedFile result;
    result.requested_path = request.requested_path;
    const auto rate = request.engine_sample_rate ? request.engine_sample_rate
                                                 : decoded.sample_rate;
    const auto plan =
            request.branches && branch_preparation_supported(*request.branches,
                                        rate, decoded.channels.size(),
                                        decoded.sample_rate)
                    ? rate_plan(*request.branches, rate, decoded.sample_rate)
                    : std::nullopt;
    result.convolver = PairedConvolver::prepare(
            prepare_response(decoded.channels, decoded.sample_rate, request),
            plan ? plan->divisor : 1);
    return result;
}

void PreparedFile::exchange(PairedConvolver &target,
        std::optional<std::string> &target_path) noexcept {
    convolver.swap(target);
    requested_path.swap(target_path);
}

} // namespace ekphore::irc
#if defined(__clang__)
#pragma clang fp contract(on)
#elif defined(__GNUC__)
#pragma GCC pop_options
#endif

#include "irc_staged.h"

#include "../../dsp/profile.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>

namespace ekphore::irc {

namespace diagnostic {
struct ComparisonState {
    PairedConvolver shadow;
    std::shared_ptr<ComparisonRow> row;
    void observe(float left, float right, std::pair<float, float> actual) {
        const auto other = shadow.process(left, right);
        ++row->calls;
        const std::array<float, 2> a{ actual.first, actual.second },
                b{ other.first, other.second };
        for (unsigned c = 0; c < 2; ++c) {
            row->different_samples += std::bit_cast<std::uint32_t>(a[c]) !=
                                      std::bit_cast<std::uint32_t>(b[c]);
            if (!std::isfinite(a[c]) || !std::isfinite(b[c])) {
                ++row->nonfinite_samples;
                continue;
            }
            const double error = double(a[c]) - b[c];
            row->maximum_error[c] =
                    std::max(row->maximum_error[c], std::abs(error));
            row->square_error[c] += error * error;
            row->actual_square[c] += double(a[c]) * a[c];
            row->shadow_square[c] += double(b[c]) * b[c];
        }
    }
};
} // namespace diagnostic
PairedConvolver::PairedConvolver() = default;
PairedConvolver::~PairedConvolver() = default;
PairedConvolver::PairedConvolver(PairedConvolver &&) noexcept = default;
PairedConvolver &PairedConvolver::operator=(
        PairedConvolver &&) noexcept = default;
bool PairedConvolver::configured() const noexcept {
    return staged ? staged->configured() : !impulses.empty();
}
std::size_t PairedConvolver::impulse_frames() const noexcept {
    return staged ? staged->impulse_frames()
                  : response_frames * convolution_rate.divisor();
}
unsigned PairedConvolver::rate_divisor() const noexcept {
    return staged ? staged->rate_divisor() : convolution_rate.divisor();
}
PairedConvolver::Backend PairedConvolver::backend() const noexcept {
    return staged ? Backend::staged_ordinary : Backend::legacy;
}

PairedConvolver::Backend PairedConvolver::default_backend() noexcept {
#if defined(EKPHORE_EXPERIMENTAL_STAGED_IRC)
    return Backend::staged_ordinary;
#else
    return Backend::legacy;
#endif
}

PairedConvolver::PairedConvolver(const PairedConvolver &other)
    : convolution_rate(other.convolution_rate),
      staged(other.staged ? std::make_unique<StagedPairedConvolver>(
                                    other.staged->clone())
                          : nullptr),
      comparison(other.comparison
                         ? std::make_unique<diagnostic::ComparisonState>(
                                   *other.comparison)
                         : nullptr),
      tails(other.tails), response_frames(other.response_frames),
      impulses(other.impulses), left_history(other.left_history),
      right_history(other.right_history), cursor(other.cursor) {}
PairedConvolver &PairedConvolver::operator=(const PairedConvolver &other) {
    if (this != &other) {
        PairedConvolver replacement(other);
        swap(replacement);
    }
    return *this;
}

void apply_envelope(
        std::span<std::vector<float>> channels, const Envelope &envelope) {
    if (!envelope.active || channels.empty())
        return;
    const auto frames = channels.front().size();
    if (static_cast<float>(frames) >= 0x1p31F ||
            std::ranges::any_of(channels,
                    [frames](const auto &c) { return c.size() != frames; }))
        throw std::invalid_argument("invalid IRC envelope plane extent");
    if (!frames)
        return;
    struct Point {
        float time, level;
    };
    std::array<Point, 8> points{};
    for (std::size_t i = 0; i < points.size(); ++i) {
        if (!std::isfinite(envelope.times[i]) ||
                !std::isfinite(envelope.levels[i]))
            throw std::invalid_argument("nonfinite IRC envelope point");
        points[i] = { envelope.times[i], envelope.levels[i] };
    }
    // swap with the first later smaller point and retry, clamp AFTER. not a
    // stable sort
    for (std::size_t i = 0; i + 1 < points.size();) {
        auto j = i + 1;
        while (j < points.size() && points[i].time <= points[j].time)
            ++j;
        if (j == points.size())
            ++i;
        else
            std::swap(points[i], points[j]);
    }
    // volatile so nothing fuses here
    const auto mul = [](float a, float b) {
        volatile float r = a * b;
        return r;
    };
    const auto add = [](float a, float b) {
        volatile float r = a + b;
        return r;
    };
    std::array<std::size_t, 8> indices{};
    std::array<float, 8> gains{};
    for (std::size_t i = 0; i < points.size(); ++i) {
        const float time = std::clamp(points[i].time, 0.F, 1.F);
        const float position = add(mul(time, static_cast<float>(frames)), .5F);
        if (position >= 0x1p31F)
            throw std::invalid_argument(
                    "IRC envelope index outside signed range");
        indices[i] =
                static_cast<std::size_t>(static_cast<std::int32_t>(position));
        if (indices[i] > frames)
            throw std::invalid_argument(
                    "IRC envelope rounded index outside plane");
        gains[i] = std::exp(
                mul(mul(points[i].level, std::bit_cast<float>(0x3d4ccccdU)),
                        std::bit_cast<float>(0x40135d8eU)));
        if (!std::isfinite(gains[i]))
            throw std::invalid_argument(
                    "IRC envelope gain outside finite range");
    }
    // half open segments, (delta * float(j)) * inverseLength + gain0, then
    // gain * input
    for (std::size_t i = 0; i + 1 < points.size(); ++i) {
        const auto length = indices[i + 1] - indices[i];
        if (!length)
            continue;
        volatile float inverse = 1.F / static_cast<float>(length);
        const float delta = add(gains[i + 1], -gains[i]);
        for (std::size_t j = 0; j < length; ++j) {
            const float gain = add(
                    gains[i], mul(mul(delta, static_cast<float>(j)), inverse));
            for (auto &channel : channels)
                channel[indices[i] + j] = mul(gain, channel[indices[i] + j]);
        }
    }
}

std::size_t pre_delay_frames(
        const float milliseconds, const std::uint32_t processing_rate) {
    // truncate the delay in processing-rate frames before prefixing the
    // response
    const float frames =
            milliseconds * static_cast<float>(processing_rate) * 0.001F;
    if (!std::isfinite(frames) || frames < 0.0F ||
            frames >= static_cast<float>(
                              std::numeric_limits<std::int32_t>::max()))
        throw std::invalid_argument("invalid IRC pre-delay extent");
    return static_cast<std::size_t>(frames);
}

float auto_gain(std::span<const std::vector<float>> channels) {
#if defined(__clang__)
#pragma clang fp contract(off)
#endif
    // fused float in order, NOT a double sum
    float maximum_energy = 0.0F;
    for (const auto &channel : channels) {
        float energy = 0.0F;
        for (const float sample : channel)
            energy = std::fma(sample, sample, energy);
        maximum_energy = std::max(maximum_energy, energy);
    }
    if (maximum_energy < 0.001)
        return 1.0F;
    return std::min(2.0F, std::sqrt(1.0F / (maximum_energy + maximum_energy)));
}

std::vector<std::vector<float>> prepare_low_latency_ir(
        std::span<const std::vector<float>> channels,
        const std::size_t convolution_block_size, const bool apply_auto_gain,
        const std::size_t pre_delay_frames) {
    const float gain = apply_auto_gain ? auto_gain(channels) : 1.0F;
    std::vector<std::vector<float>> prepared;
    prepared.reserve(channels.size());
    for (const auto &channel : channels) {
        auto &destination = prepared.emplace_back(
                convolution_block_size + pre_delay_frames + channel.size(),
                0.0F);
        std::transform(channel.begin(), channel.end(),
                destination.begin() +
                        static_cast<std::ptrdiff_t>(
                                convolution_block_size + pre_delay_frames),
                [gain](const float sample) { return sample * gain; });
    }
    return prepared;
}

void PairedConvolver::configure(
        std::vector<std::vector<float>> impulse_channels) {
    auto replacement = prepare(std::move(impulse_channels));
    swap(replacement);
}

void PairedConvolver::swap(PairedConvolver &other) noexcept {
    staged.swap(other.staged);
    comparison.swap(other.comparison);
    convolution_rate.swap(other.convolution_rate);
    tails.swap(other.tails);
    impulses.swap(other.impulses);
    left_history.swap(other.left_history);
    right_history.swap(other.right_history);
    std::swap(cursor, other.cursor);
    std::swap(response_frames, other.response_frames);
}

PairedConvolver PairedConvolver::prepare(
        std::vector<std::vector<float>> impulse_channels, unsigned rate_divisor,
        Backend backend) {
    PairedConvolver result;
    if (backend == Backend::build_default) {
        backend = default_backend();
    }
    if (auto *collector = diagnostic::active_comparison) {
        // no diagnostics while we build the shadow
        struct Suspend {
            diagnostic::ComparisonCollector *previous =
                    diagnostic::active_comparison;
            Suspend() { diagnostic::active_comparison = nullptr; }
            ~Suspend() { diagnostic::active_comparison = previous; }
        } suspend;
        auto row = std::make_shared<diagnostic::ComparisonRow>();
        row->channels = impulse_channels.size();
        row->divisor = rate_divisor;
        row->actual_staged = backend == Backend::staged_ordinary;
        for (const auto &channel : impulse_channels)
            row->ir_frames = std::max(row->ir_frames, channel.size());
        auto shadow = prepare(impulse_channels, rate_divisor,
                row->actual_staged ? Backend::legacy
                                   : Backend::staged_ordinary);
        result.comparison = std::make_unique<diagnostic::ComparisonState>(
                diagnostic::ComparisonState{ std::move(shadow), row });
        collector->rows.push_back(std::move(row));
    }
    if (backend == Backend::staged_ordinary) {
        result.staged = std::make_unique<StagedPairedConvolver>(
                StagedPairedConvolver::prepare(
                        std::move(impulse_channels), rate_divisor));
        return result;
    }
    result.convolution_rate = PairedConvolutionRate::prepare(rate_divisor);
    auto &tails = result.tails;
    auto &impulses = result.impulses;
    auto &left_history = result.left_history;
    auto &right_history = result.right_history;
    constexpr std::size_t head_size = 128;
    constexpr std::size_t maximum_block = 1024;
    tails.clear();
    std::size_t length = 0;
    for (const auto &channel : impulse_channels)
        length = std::max(length, channel.size());
    result.response_frames = length;
    for (std::size_t block = head_size; block < length; block *= 2) {
        auto &tail = tails.emplace_back();
        tail.size = block;
        tail.partitions = block == maximum_block
                                  ? (length - block + block - 1) / block
                                  : 1;
        const auto n = block * 2;
        std::vector<std::uint32_t> permutation(n);
        tail.roots.resize(n / 2);
        native::fill_bit_reversal(permutation);
        // only the swaps the scan did, same order
        tail.permutation_swaps.reserve(n / 2);
        for (std::uint32_t i = 0; i < n; ++i)
            if (i < permutation[i])
                tail.permutation_swaps.emplace_back(i, permutation[i]);
        for (std::size_t i = 0; i < n / 2; ++i) {
            const double angle = -2.0 * std::numbers::pi * i / n;
            tail.roots[i] = { static_cast<float>(std::cos(angle)),
                static_cast<float>(std::sin(angle)) };
        }
        for (std::size_t channel = 0; channel < 2; ++channel) {
            const auto &ir = impulse_channels[std::min(
                    channel, impulse_channels.size() - 1)];
            tail.input[channel].resize(block);
            tail.output[channel].resize(n);
            tail.work[channel].resize(n);
            tail.history[channel].resize(n * tail.partitions);
            tail.spectra[channel].resize(n * tail.partitions);
            auto &work = tail.work[channel];
            for (std::size_t p = 0; p < tail.partitions; ++p) {
                std::fill(work.begin(), work.end(), Tail::Complex{});
                for (std::size_t i = 0; i < block; ++i) {
                    const auto index = block + p * block + i;
                    if (index < ir.size())
                        work[i] = ir[index];
                }
                tail.transform(work, false);
                std::copy(work.begin(), work.end(),
                        tail.spectra[channel].begin() + p * n);
            }
        }
        tail.reset();
        if (block == maximum_block)
            break;
    }
    for (auto &channel : impulse_channels)
        if (channel.size() > head_size)
            channel.resize(head_size);
    impulses = std::move(impulse_channels);
    std::size_t history_size = 0;
    for (const auto &channel : impulses)
        history_size = std::max(history_size, channel.size());
    left_history.assign(history_size, 0.0F);
    right_history.assign(history_size, 0.0F);
    return result;
}

void PairedConvolver::reset() {
    if (comparison)
        comparison->shadow.reset();
    if (staged) {
        staged->reset();
        return;
    }
    convolution_rate.reset();
    for (auto &tail : tails)
        tail.reset();
    std::fill(left_history.begin(), left_history.end(), 0.0F);
    std::fill(right_history.begin(), right_history.end(), 0.0F);
    cursor = 0;
}

void PairedConvolver::Tail::transform(
        std::vector<Complex> &data, bool inverse) const {
    const auto n = data.size();
    for (const auto [first, second] : permutation_swaps)
        std::swap(data[first], data[second]);
    for (std::size_t width = 2; width <= n; width *= 2) {
        const auto half = width / 2;
        const auto stride = n / width;
        for (std::size_t start = 0; start < n; start += width) {
            for (std::size_t i = 0; i < half; ++i) {
                const auto root = inverse ? std::conj(roots[i * stride])
                                          : roots[i * stride];
                const auto a = data[start + i];
                const auto b = data[start + i + half] * root;
                data[start + i] = a + b;
                data[start + i + half] = a - b;
            }
        }
    }
    if (inverse)
        for (auto &value : data)
            value /= static_cast<float>(n);
}

void PairedConvolver::Tail::reset() {
    input_cursor = output_cursor = slot = 0;
    for (std::size_t c = 0; c < 2; ++c) {
        std::fill(input[c].begin(), input[c].end(), 0.0F);
        std::fill(output[c].begin(), output[c].end(), 0.0F);
        std::fill(history[c].begin(), history[c].end(), Complex{});
        std::fill(work[c].begin(), work[c].end(), Complex{});
    }
}

std::pair<float, float> PairedConvolver::Tail::process(
        float left, float right) {
    const auto result =
            std::pair(output[0][output_cursor], output[1][output_cursor]);
    output[0][output_cursor] = output[1][output_cursor] = 0.0F;
    input[0][input_cursor] = left;
    input[1][input_cursor] = right;
    const auto n = size * 2;
    if (++output_cursor == n)
        output_cursor = 0;
    if (++input_cursor == size) {
        input_cursor = 0;
        for (std::size_t c = 0; c < 2; ++c) {
            auto &scratch = work[c];
            dsp::profile::Scope forward_timing(
                    dsp::profile::convolution_key(
                            size, dsp::profile::ConvolutionStage::forward),
                    n);
            for (std::size_t i = 0; i < size; ++i)
                scratch[i] = input[c][i];
            std::fill(scratch.begin() + size, scratch.end(), Complex{});
            transform(scratch, false);
            std::copy(scratch.begin(), scratch.end(),
                    history[c].begin() + slot * n);
            forward_timing.finish();
            dsp::profile::Scope multiply_timing(
                    dsp::profile::convolution_key(
                            size, dsp::profile::ConvolutionStage::multiply),
                    n * partitions);
            std::fill(scratch.begin(), scratch.end(), Complex{});
            auto history_slot = slot;
            for (std::size_t p = 0; p < partitions; ++p) {
                const auto *x = history[c].data() + history_slot * n;
                const auto *h = spectra[c].data() + p * n;
                // plain complex multiply in partition order, no reordering or
                // SIMD
                std::size_t i = 0;
                for (; i + 4 <= n; i += 4) {
                    const std::array<Complex, 4> products{ x[i] * h[i],
                        x[i + 1] * h[i + 1], x[i + 2] * h[i + 2],
                        x[i + 3] * h[i + 3] };
                    for (std::size_t lane = 0; lane < products.size(); ++lane)
                        scratch[i + lane] += products[lane];
                }
                for (; i < n; ++i)
                    scratch[i] += x[i] * h[i];
                history_slot =
                        history_slot == 0 ? partitions - 1 : history_slot - 1;
            }
            multiply_timing.finish();
            dsp::profile::Scope inverse_timing(
                    dsp::profile::convolution_key(
                            size, dsp::profile::ConvolutionStage::inverse),
                    n);
            transform(scratch, true);
            inverse_timing.finish();
            dsp::profile::Scope overlap_timing(
                    dsp::profile::convolution_key(
                            size, dsp::profile::ConvolutionStage::overlap),
                    n);
            // same order, split at the one wrap instead of a modulo per bin
            const auto first = n - output_cursor;
            for (std::size_t i = 0; i < first; ++i)
                output[c][output_cursor + i] += scratch[i].real();
            for (std::size_t i = first; i < n; ++i)
                output[c][i - first] += scratch[i].real();
        }
        if (++slot == partitions)
            slot = 0;
    }
    return result;
}

float PairedConvolver::convolve(const std::size_t impulse_channel,
        const std::size_t input_channel) const {
    if (impulses.empty() || left_history.empty())
        return 0.0F;
    const auto &impulse =
            impulses[std::min(impulse_channel, impulses.size() - 1)];
    const auto &history = input_channel == 0 ? left_history : right_history;
    double output = 0.0;
    // double accumulator in order, split at the wrap so there's no branch per
    // tap
    const auto first = std::min(impulse.size(), cursor + 1);
    for (std::size_t tap = 0; tap < first; ++tap)
        output += static_cast<double>(impulse[tap]) * history[cursor - tap];
    for (std::size_t tap = first; tap < impulse.size(); ++tap)
        output += static_cast<double>(impulse[tap]) *
                  history[history.size() - 1 - (tap - first)];
    return static_cast<float>(output);
}

std::pair<float, float> PairedConvolver::process(
        const float left, const float right) {
    const auto output = staged ? staged->process(left, right)
                               : convolution_rate.process(
                                         left, right, [this](float l, float r) {
                                             return process_core(l, r);
                                         });
    if (comparison)
        comparison->observe(left, right, output);
    return output;
}

std::pair<float, float> PairedConvolver::process_core(float left, float right) {
    if (left_history.empty())
        return {};
    left_history[cursor] = left;
    right_history[cursor] = right;
    float output_left = convolve(0, 0);
    float output_right = convolve(impulses.size() > 1 ? 1 : 0, 1);
    for (auto &tail : tails) {
        const auto output = tail.process(left, right);
        output_left += output.first;
        output_right += output.second;
    }
    if (++cursor == left_history.size())
        cursor = 0;
    return { output_left, output_right };
}

} // namespace ekphore::irc

// merging also needs separate float operations
#if defined(__clang__)
#pragma clang fp contract(off)
#elif defined(__GNUC__)
#pragma GCC push_options
#pragma GCC optimize("fp-contract=off")
#endif
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <stdexcept>

namespace ekphore::irc {
namespace {
float irc_merge_mul(float a, float b) {
    volatile float r = a * b;
    return r;
}
float irc_merge_add(float a, float b) {
    volatile float r = a + b;
    return r;
}
float irc_merge_div(float a, float b) {
    volatile float r = a / b;
    return r;
}
std::int64_t irc_merge_trunc_index(float x) {
    if (!std::isfinite(x) || x < -0x1p31F || x >= 0x1p31F)
        throw std::invalid_argument("IRC merge signed index extent");
    return static_cast<std::int32_t>(x);
}
} // namespace
std::vector<std::vector<float>> merge_early_late(
        std::span<const std::vector<float>> early,
        std::span<const std::vector<float>> late, const MergeParameters &p) {
    if (early.empty() || early.size() != late.size() ||
            !std::isfinite(p.sample_rate) || p.sample_rate <= 0.F ||
            !std::isfinite(p.processing_rate) || p.processing_rate <= 0.F ||
            !std::isfinite(p.early_stretch) || p.early_stretch <= 0.F ||
            !std::isfinite(p.late_stretch) || p.late_stretch <= 0.F ||
            !std::isfinite(p.crossfade_seconds) || p.crossfade_seconds < 0.F ||
            p.adapted_frames >= std::size_t(INT32_MAX) ||
            p.crosspoint > p.adapted_frames)
        throw std::invalid_argument("IRC merge parameter domain");
    // lane 0 late, lane 1 early, every op in float and truncate even negatives
    const float e = irc_merge_div(
            irc_merge_mul(p.early_stretch, p.processing_rate), p.sample_rate);
    const float l = irc_merge_div(
            irc_merge_mul(p.late_stretch, p.processing_rate), p.sample_rate);
    const float W = float(irc_merge_trunc_index(irc_merge_add(
            irc_merge_mul(p.sample_rate, p.crossfade_seconds), .5F)));
    const float half = irc_merge_mul(W, .5F);
    const float ec = irc_merge_mul(e, float(p.crosspoint)),
                lc = irc_merge_mul(l, float(p.crosspoint));
    const auto raw_es =
            irc_merge_trunc_index(irc_merge_add(irc_merge_add(ec, -half), .5F));
    const auto es = std::max<std::int64_t>(0, raw_es);
    const auto ls = std::max<std::int64_t>(
            0, irc_merge_trunc_index(
                       irc_merge_add(irc_merge_add(lc, -half), .5F)));
    const auto en = irc_merge_trunc_index(
            irc_merge_add(irc_merge_mul(e, float(p.adapted_frames)), .5F));
    const auto ln = irc_merge_trunc_index(
            irc_merge_add(irc_merge_mul(l, float(p.adapted_frames)), .5F));
    const auto ee = std::min(en,
            irc_merge_trunc_index(irc_merge_add(irc_merge_add(ec, half), .5F)));
    const auto le = std::min(ln,
            irc_merge_trunc_index(irc_merge_add(irc_merge_add(lc, half), .5F)));
    const auto ed = ee - es, ld = le - ls;
    const auto a = ls + ld - ed, b = es + ed - ld;
    const auto out_start = ld >= ed ? std::max<std::int64_t>(b, 0)
                                    : es - std::min<std::int64_t>(a, 0);
    const auto late_start = ld >= ed ? ls - std::min<std::int64_t>(b, 0)
                                     : std::max<std::int64_t>(a, 0);
    const auto length = ee - le + ln;
    const auto overlap = std::max<std::int64_t>(0, ee - out_start);
    if (ee < 0 || le < 0 || ln < le || length < ee || length >= INT32_MAX ||
            late_start < 0 || (overlap && W == 0.F))
        throw std::invalid_argument("unqualified IRC merge clipping geometry");
    // check every range BEFORE processing
    for (std::size_t ch = 0; ch < early.size(); ++ch) {
        if (early[ch].size() < std::size_t(ee) ||
                late[ch].size() < std::size_t(ln) ||
                (overlap &&
                        late[ch].size() < std::size_t(late_start + overlap)))
            throw std::invalid_argument("IRC merge prepared plane capacity");
    }
    std::vector<std::vector<float>> result(
            early.size(), std::vector<float>(length, 0.F));
    for (std::size_t ch = 0; ch < early.size(); ++ch) {
        std::copy_n(early[ch].begin(), ee, result[ch].begin());
        std::copy(late[ch].begin() + le, late[ch].begin() + ln,
                result[ch].begin() + ee);
    }
    for (auto i = out_start, j = late_start; i < ee; ++i, ++j) {
        const auto q = i - raw_es;
        const float w = std::cos(irc_merge_div(
                irc_merge_mul(float(q), std::bit_cast<float>(0x3fc90fdbU)), W));
        const float w2 = irc_merge_mul(w, w),
                    complement = irc_merge_add(1.F, -w2);
        for (std::size_t ch = 0; ch < early.size(); ++ch)
            result[ch][i] = irc_merge_add(irc_merge_mul(w2, result[ch][i]),
                    irc_merge_mul(complement, late[ch][j]));
    }
    return result;
}
} // namespace ekphore::irc
#if defined(__clang__)
#pragma clang fp contract(on)
#elif defined(__GNUC__)
#pragma GCC pop_options
#endif
