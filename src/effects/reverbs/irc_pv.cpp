// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "irc_pv.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <new>
#include <stdexcept>
#if defined(__APPLE__)
#include <Accelerate/Accelerate.h>
#endif

namespace ekphore::irc::pv {
FreshCoefficients prepare_fresh_coefficients(std::size_t K, std::size_t N,
        std::size_t analysisHop, std::size_t normalizationHop) {
    if (K < 2 || K % 2 || K > std::size_t(INT32_MAX / 2) || N < 4 ||
            !std::has_single_bit(N) || N > std::size_t(INT32_MAX) ||
            analysisHop == 0 || analysisHop > K || normalizationHop == 0 ||
            normalizationHop > std::size_t(INT32_MAX))
        throw std::invalid_argument("unqualified IRC PV coefficient geometry");

    // volatile so nothing fuses, especially the double affine
    constexpr float pi = std::bit_cast<float>(0x40490fdbU);
    constexpr float tau = std::bit_cast<float>(0x40c90fdbU);
    constexpr float inverse_log_two = std::bit_cast<float>(0x3fb8aa3bU);
    constexpr double cosine_weight =
            std::bit_cast<double>(0xbfdd8b827fa1a0cfULL);
    constexpr double constant = std::bit_cast<double>(0x3fe13a3ec02f2f98ULL);
    volatile float ratio = float(K) / float(analysisHop);
    volatile float argument = ratio + 1.F;
    volatile float logarithm = std::log(float(argument));
    volatile float exponent_value = logarithm * inverse_log_two;
    const int exponent = std::min(3, int(std::floor(float(exponent_value))));
    volatile float step = pi / float(K - 1);
    FreshCoefficients result{ std::vector<float>(K), std::vector<float>(N),
        std::vector<float>(K) };
    for (std::size_t i = 0; i < K / 2; ++i) {
        volatile float angle = step * float(2 * i);
        const float cosine = std::cos(float(angle));
        volatile double product = double(cosine) * cosine_weight;
        volatile double affine = product + constant;
        const float base = float(affine);
        float value = base;
        for (int p = 1; p < exponent; ++p) {
            volatile float powered = value * base;
            value = powered;
        }
        result.window[i] = result.window[K - 1 - i] = value;
    }
    volatile float hop_tau = float(analysisHop) * tau;
    volatile float scale = hop_tau / float(N);
    for (std::size_t i = 0; i < N; ++i) {
        volatile float phase = scale * float(i);
        result.phase_step[i] = phase;
    }
    const auto count = K / normalizationHop;
    for (std::size_t i = 0; i < K; ++i) {
        volatile float sum = result.window[i] * result.window[i];
        for (std::size_t t = 1; t <= count; ++t) {
            // equal wraps K to window[0]
            if (i <= K - t * normalizationHop) {
                const auto j = (i + t * normalizationHop) % K;
                volatile float square = result.window[j] * result.window[j];
                volatile float added = sum + square;
                sum = added;
            }
        }
        volatile float reciprocal = sum == 0.F ? 1.F : 1.F / sum;
        result.normalization[i] = reciprocal;
    }
    return result;
}

float principal_argument(float value) noexcept {
    // volatile, same reason
    constexpr float pi = std::bit_cast<float>(0x40490fdbU);
    constexpr float negative_inverse_tau = std::bit_cast<float>(0xbe22f983U);
    constexpr float tau = std::bit_cast<float>(0x40c90fdbU);
    volatile float shifted = value + pi;
    volatile float scaled = shifted * negative_inverse_tau;
    const float turns = std::floor(scaled);
    volatile float offset = turns * tau;
    volatile float wrapped = shifted + offset;
    return wrapped + pi;
}
void principal_argument(std::span<float> values) noexcept {
    for (auto &value : values)
        value = principal_argument(value);
}

#if defined(__APPLE__)
namespace {
std::size_t irc_pv_validated_size(std::size_t n) {
    if (n < 4 || !std::has_single_bit(n) || n > std::size_t(INT32_MAX))
        throw std::invalid_argument(
                "IRC PV FFT requires a power-of-two size >=4");
    return n;
}
} // namespace
RealFft::RealFft(std::size_t size)
    : fft_size(irc_pv_validated_size(size)),
      log2_size(std::countr_zero(fft_size)),
      real(static_cast<float *>(::operator new(
              (fft_size / 2) * sizeof(float), std::align_val_t{ 256 }))),
      imaginary(static_cast<float *>(::operator new(
              (fft_size / 2) * sizeof(float), std::align_val_t{ 256 }))),
      scratch(static_cast<float *>(::operator new(
              fft_size * sizeof(float), std::align_val_t{ 256 }))) {
    // 256 aligned, vDSP picks different kernels otherwise and the results
    // change
    std::fill_n(real.get(), fft_size / 2, 0.F);
    std::fill_n(imaginary.get(), fft_size / 2, 0.F);
    std::fill_n(scratch.get(), fft_size, 0.F);
    // radix 2, NOT the convolver's FFT
    setup = vDSP_create_fftsetup(log2_size, kFFTRadix2);
    if (!setup)
        throw std::runtime_error("IRC PV FFT setup failed");
}
void RealFft::PlaneDeleter::operator()(float *p) const noexcept {
    ::operator delete(p, std::align_val_t{ 256 });
}
RealFft::~RealFft() { vDSP_destroy_fftsetup(static_cast<FFTSetup>(setup)); }
void RealFft::forward(std::span<float> data) {
    if (data.size() != fft_size)
        throw std::invalid_argument("IRC PV FFT plane extent");
    DSPSplitComplex split{ real.get(), imaginary.get() };
    // the PV does the window and the 1/(N/2)
    vDSP_ctoz(reinterpret_cast<const DSPComplex *>(data.data()), 2, &split, 1,
            fft_size / 2);
    vDSP_fft_zrip(
            static_cast<FFTSetup>(setup), &split, 1, log2_size, FFT_FORWARD);
    std::copy_n(real.get(), fft_size / 2, data.begin());
    std::copy_n(imaginary.get(), fft_size / 2, data.begin() + fft_size / 2);
    const float half = .5F;
    vDSP_vsmul(data.data(), 1, &half, data.data(), 1, fft_size);
}
void RealFft::inverse(std::span<float> data) {
    if (data.size() != fft_size)
        throw std::invalid_argument("IRC PV FFT plane extent");
    std::copy_n(data.begin(), fft_size / 2, real.get());
    std::copy_n(data.begin() + fft_size / 2, fft_size / 2, imaginary.get());
    DSPSplitComplex split{ real.get(), imaginary.get() };
    vDSP_fft_zrip(
            static_cast<FFTSetup>(setup), &split, 1, log2_size, FFT_INVERSE);
    vDSP_ztoc(&split, 1, reinterpret_cast<DSPComplex *>(scratch.get()), 2,
            fft_size / 2);
    std::copy_n(scratch.get(), fft_size, data.begin());
    // no 1/N here, the PV does the .5 itself
}
#endif
} // namespace ekphore::irc::pv

#include <algorithm>
#include <bit>
#include <cmath>
#include <stdexcept>
#if defined(__APPLE__)
#include <Accelerate/Accelerate.h>

namespace ekphore::irc::pv {
namespace {
std::size_t irc_pv_frame_geometry(std::size_t n) {
    if (n < 512 || n > 32768 || !std::has_single_bit(n))
        throw std::invalid_argument("unqualified IRC PV frame geometry");
    return n;
}
void irc_pv_frame_multiply(float *dst, const float *src, std::size_t n) {
    vDSP_vmul(dst, 1, src, 1, dst, 1, n);
}
void irc_pv_frame_add(float *dst, const float *src, std::size_t n) {
    vDSP_vadd(dst, 1, src, 1, dst, 1, n);
}
void irc_pv_frame_scale(float *dst, float value, std::size_t n) {
    vDSP_vsmul(dst, 1, &value, dst, 1, n);
}
std::span<const float> irc_pv_frame_half(
        const std::vector<float> &v, std::size_t h) {
    return { v.data(), h };
}
std::span<float> irc_pv_frame_half(std::vector<float> &v, std::size_t h) {
    return { v.data(), h };
}
} // namespace

// clang-format off
FrameProcessor::FrameProcessor(std::size_t size, std::size_t a, std::size_t out,
        std::size_t norm, std::size_t channels, float stretch) :
        spectral_size(irc_pv_frame_geometry(size)), half_size(spectral_size /
        2), analysis_stride(a), synthesis_hop(out), normalization_stride(norm),
        bin_limit(half_size), stretch_ratio(stretch), last_stretch(stretch),
        coefficients(prepare_fresh_coefficients(spectral_size, spectral_size,
        analysis_stride, norm)), fft(spectral_size), regions(spectral_size),
        work(spectral_size), magnitude(half_size + 1),
        quarter_history(half_size + 1), regular_history(half_size + 1),
        phase_values(half_size + 1), synthesis_values(half_size + 1),
        previous_phase(half_size), retained_synthesis(half_size),
        frequency_values(half_size), offset(half_size), cosine(half_size),
        sine(half_size), flags(half_size), ratios(half_size),
        transient_work(spectral_size), peak_indices(half_size) {
    // clang-format on
    if (!channels || channels > 48 || !out || out > spectral_size || !norm ||
            norm > spectral_size || !std::isfinite(stretch) || stretch <= 0.F)
        throw std::invalid_argument("IRC PV frame dimensions/stretch");
    channel_data.reserve(channels);
    for (std::size_t ch = 0; ch < channels; ++ch)
        channel_data.push_back({ std::vector<float>(2 * spectral_size),
                std::vector<float>(spectral_size + 1),
                std::vector<float>(spectral_size + 1),
                std::vector<float>(2 * spectral_size),
                std::vector<float>(half_size), std::vector<float>(half_size) });
    // the initial stretch has to come from you
}
bool FrameProcessor::cutoff_supported() noexcept { return true; }
void FrameProcessor::set_cutoff(std::size_t m) {
    if (!m || m > half_size)
        throw std::invalid_argument(
                "IRC PV cutoff outside qualified Regions domain");
    bin_limit = m;
}
void FrameProcessor::set_stretch(float value) {
    if (!std::isfinite(value) || value <= 0.F)
        throw std::invalid_argument("IRC PV stretch");
    stretch_ratio = value;
}
void FrameProcessor::set_hops(
        unsigned out, unsigned a, unsigned norm, float stretch) {
    if (!a || a > spectral_size || !out || out > spectral_size || !norm ||
            norm > spectral_size || !std::isfinite(stretch) || stretch <= 0.F)
        throw std::invalid_argument("IRC PV hop publication");
    // same hops and already started means the cache wins, output hop and
    // stretch don't matter
    if (analysis_stride == a && normalization_stride == norm &&
            processed_count != 0)
        return;
    auto prepared =
            prepare_fresh_coefficients(spectral_size, spectral_size, a, norm);
    // new tables first, then fix the overlap with the OLD normalization
    if (processed_count != 0)
        for (auto &ch : channel_data)
            irc_pv_frame_multiply(ch.overlap.data() + spectral_size,
                    coefficients.normalization.data(), spectral_size);
    if (analysis_stride != a) {
        coefficients.window.swap(prepared.window);
        coefficients.phase_step.swap(prepared.phase_step);
    }
    coefficients.normalization.swap(prepared.normalization);
    if (processed_count != 0) {
        // vDSP_vdiv(src,...) is dst/src, don't turn it into a reciprocal
        // multiply
        for (auto &ch : channel_data) {
            auto *ola = ch.overlap.data() + spectral_size;
            vDSP_vdiv(coefficients.normalization.data(), 1, ola, 1, ola, 1,
                    spectral_size);
        }
    }
    analysis_stride = a;
    normalization_stride = norm;
    synthesis_hop = out;
    stretch_ratio = stretch;
}
void FrameProcessor::reset() {
    regions.reset_tables();
    bin_limit = half_size;
    last_stretch = 1.F;
    for (auto &ch : channel_data) {
        std::fill(ch.input.begin(), ch.input.end(), 0.F);
        std::fill_n(ch.spectrum.begin(), spectral_size, 0.F);
        std::fill_n(ch.reconstructed.begin(), spectral_size, 0.F);
        std::fill(ch.overlap.begin(), ch.overlap.end(), 0.F);
        std::fill(ch.phase.begin(), ch.phase.end(), 0.F);
        std::fill(ch.last_phase.begin(), ch.last_phase.end(), 0.F);
    }
    for (auto *v : { &work, &magnitude, &quarter_history, &regular_history,
                 &phase_values, &synthesis_values, &frequency_values, &offset,
                 &cosine, &sine, &flags, &ratios })
        std::fill(v->begin(), v->end(), 0.F);
    std::fill(transient_work.begin(), transient_work.end(), 0);
    std::fill(peak_indices.begin(), peak_indices.end(), 0);
}
void FrameProcessor::validate_input(
        std::span<const std::span<const float>> input) const {
    if (input.size() != channel_data.size())
        throw std::invalid_argument("IRC PV frame input channels");
    for (auto lane : input)
        if (lane.size() != analysis_stride ||
                !std::ranges::all_of(
                        lane, [](float x) { return std::isfinite(x); }))
            throw std::invalid_argument(
                    "IRC PV frame input extent/finite domain");
}
void FrameProcessor::append(Channel &ch, std::span<const float> input) {
    std::move(ch.input.begin() + analysis_stride,
            ch.input.begin() + spectral_size, ch.input.begin());
    std::copy(input.begin(), input.end(),
            ch.input.begin() + spectral_size - analysis_stride);
}
void FrameProcessor::prime(std::span<const std::span<const float>> input) {
    validate_input(input);
    for (std::size_t ch = 0; ch < channel_data.size(); ++ch)
        append(channel_data[ch], input[ch]);
}
unsigned FrameProcessor::prime_channel(
        std::size_t channel, std::span<const float> input) {
    if (channel >= channel_data.size() || input.size() != analysis_stride ||
            !std::ranges::all_of(
                    input, [](float x) { return std::isfinite(x); }))
        throw std::invalid_argument("IRC PV channel warmup");
    append(channel_data[channel], input);
    return unsigned(synthesis_hop);
}
void FrameProcessor::analyze(Channel &ch) {
    std::fill(work.begin(), work.end(), 0.F);
    std::copy_n(ch.input.begin() + half_size, half_size, work.begin());
    irc_pv_frame_multiply(
            work.data(), coefficients.window.data() + half_size, half_size);
    std::copy_n(ch.input.begin(), half_size,
            work.begin() + spectral_size - half_size);
    irc_pv_frame_multiply(work.data() + spectral_size - half_size,
            coefficients.window.data(), half_size);
    fft.forward(work);
    std::copy(work.begin(), work.end(), ch.spectrum.begin());
    irc_pv_frame_scale(
            ch.spectrum.data(), 1.F / float(half_size), spectral_size);
}
void FrameProcessor::phase_and_magnitude(Channel &ch, std::size_t channel) {
    auto &s = ch.spectrum;
    const float nyquist = s[half_size];
    s[half_size] = 0.F;
    for (std::size_t k = 0; k < half_size; ++k)
        if (s[k] == 0.F)
            s[k] = 1e-8F;
    DSPSplitComplex split{ s.data(), s.data() + half_size };
    vDSP_zvphas(&split, 1, phase_values.data(), 1, half_size);
    s[half_size] = nyquist;
    irc_pv_frame_add(
            ch.phase.data(), coefficients.phase_step.data(), bin_limit);
    irc_pv_frame_scale(ch.phase.data(), -1.F, bin_limit);
    irc_pv_frame_add(ch.phase.data(), phase_values.data(), bin_limit);
    for (std::size_t k = 0; k < bin_limit; ++k)
        frequency_values[k] = principal_argument(ch.phase[k]);
    frequency_values[0] = 0.F;
    irc_pv_frame_add(
            frequency_values.data(), coefficients.phase_step.data(), bin_limit);
    constexpr float pi = std::bit_cast<float>(0x40490fdbU);
    phase_values[0] = s[0] >= 0.F ? 0.F : pi;
    phase_values[half_size] = s[half_size] >= 0.F ? 0.F : pi;
    if (channel == 0) {
        // rotate BEFORE writing
        regular_history.swap(quarter_history);
        quarter_history.swap(magnitude);
        const float saved = s[half_size];
        s[half_size] = 0.F;
        vDSP_zvabs(&split, 1, magnitude.data(), 1, half_size);
        s[half_size] = saved;
    }
    magnitude[0] = std::abs(s[0]) * .5F;
    magnitude[half_size] = std::abs(s[half_size]) * .5F;
}
void FrameProcessor::synthesize(
        Channel &ch, std::size_t channel, float old_stretch) {
    const auto peaks = std::span<const int>(peak_indices.data(), num_selected);
    if (num_selected) {
        if (channel == 0) {
            process_transients(spectral_size, spectral_size, analysis_stride,
                    stretch_ratio, last_stretch,
                    { regions.map(), peaks,
                            irc_pv_frame_half(magnitude, half_size),
                            irc_pv_frame_half(quarter_history, half_size),
                            irc_pv_frame_half(regular_history, half_size) },
                    { flags, ratios,
                            std::span<int>(transient_work.data(), half_size) });
            for (std::size_t ordinal = 0; ordinal < num_selected; ++ordinal) {
                const auto p = std::size_t(peaks[ordinal]);
                float predicted;
                if (flags[ordinal] < 0.F) {
                    const float product = old_stretch * frequency_values[p];
                    predicted = ch.last_phase[p] + product;
                } else {
                    const float difference = phase_values[p] - ch.last_phase[p];
                    const float residual = principal_argument(
                            difference - coefficients.phase_step[p]);
                    frequency_values[p] = coefficients.phase_step[p] + residual;
                    predicted = ch.last_phase[p] + frequency_values[p];
                }
                synthesis_values[p] = principal_argument(predicted);
                offset[p] = synthesis_values[p] - phase_values[p];
                cosine[p] = std::cos(offset[p]);
                sine[p] = std::sin(offset[p]);
            }
        } else
            synchronize_channels(spectral_size,
                    { peaks, retained_synthesis,
                            irc_pv_frame_half(phase_values, half_size),
                            previous_phase },
                    { irc_pv_frame_half(synthesis_values, half_size), offset,
                            cosine, sine });
    }
    std::fill_n(ch.reconstructed.begin(), spectral_size, 0.F);
    if (num_selected) {
        const auto map = regions.map();
        for (std::size_t k = 1; k < bin_limit; ++k) {
            const auto p = std::size_t(map[k]);
            ch.last_phase[k] = offset[p] + phase_values[k];
            const float ac = ch.spectrum[k] * cosine[p];
            const float bs = ch.spectrum[half_size + k] * sine[p];
            ch.reconstructed[k] = ac - bs;
            const float as = ch.spectrum[k] * sine[p];
            const float bc = ch.spectrum[half_size + k] * cosine[p];
            ch.reconstructed[half_size + k] = as + bc;
        }
        const float dc = magnitude[0] + magnitude[0];
        ch.reconstructed[0] = dc * std::cos(synthesis_values[0]);
        const float ny = magnitude[half_size] + magnitude[half_size];
        ch.reconstructed[half_size] =
                ny * std::cos(synthesis_values[half_size]);
    }
    std::copy_n(phase_values.begin(), half_size, ch.phase.begin());
    if (channel == 0) {
        std::copy_n(ch.phase.begin(), half_size, previous_phase.begin());
        std::copy_n(
                ch.last_phase.begin(), half_size, retained_synthesis.begin());
    }
}
void FrameProcessor::finish(Channel &ch, std::span<float> output) {
    std::copy_n(ch.reconstructed.begin(), spectral_size, work.begin());
    fft.inverse(work);
    irc_pv_frame_scale(work.data(), .5F, spectral_size);
    irc_pv_frame_multiply(work.data() + spectral_size - half_size,
            coefficients.window.data(), half_size);
    auto *ola = ch.overlap.data() + spectral_size;
    irc_pv_frame_add(ola, work.data() + spectral_size - half_size, half_size);
    irc_pv_frame_multiply(
            work.data(), coefficients.window.data() + half_size, half_size);
    irc_pv_frame_add(ola + half_size, work.data(), half_size);
    irc_pv_frame_multiply(
            ola, coefficients.normalization.data(), normalization_stride);
    std::copy_n(ola, synthesis_hop, output.begin());
    std::move(ola + synthesis_hop, ola + spectral_size, ola);
    std::fill(ola + spectral_size - synthesis_hop, ola + spectral_size, 0.F);
}
void FrameProcessor::process(std::span<const std::span<const float>> input,
        std::span<const std::span<float>> output) {
    validate_input(input);
    if (output.size() != channel_data.size())
        throw std::invalid_argument("IRC PV frame output channels");
    for (auto lane : output)
        if (lane.size() < synthesis_hop)
            throw std::invalid_argument("IRC PV frame output extent");
    for (std::size_t ch = 0; ch < channel_data.size(); ++ch) {
        for (unsigned stage = 0; stage < 5; ++stage)
            run_stage(ch, stage, input[ch], output[ch]);
    }
}
void FrameProcessor::run_stage(std::size_t channel, unsigned stage,
        std::span<const float> input, std::span<float> output) {
    if (channel >= channel_data.size() || stage > 4)
        throw std::invalid_argument("IRC PV dispatch stage/channel");
    auto &ch = channel_data[channel];
    const float entry_previous = last_stretch; // BEFORE the transients
    switch (stage) {
        case 0:
            if (input.size() != analysis_stride ||
                    !std::ranges::all_of(
                            input, [](float x) { return std::isfinite(x); }))
                throw std::invalid_argument("IRC PV stage0 input");
            append(ch, input);
            analyze(ch);
            break;
        case 1:
            phase_and_magnitude(ch, channel);
            break;
        case 2:
            if (channel == 0) {
                num_selected = regions.select(
                        irc_pv_frame_half(magnitude, half_size),
                        frequency_values, unsigned(analysis_stride), bin_limit);
                std::copy(regions.peaks().begin(), regions.peaks().end(),
                        peak_indices.begin());
                ++processed_count;
            }
            break;
        case 3:
            synthesize(ch, channel, entry_previous);
            break;
        case 4:
            if (output.size() < synthesis_hop)
                throw std::invalid_argument("IRC PV stage4 output");
            finish(ch, output);
            break;
    }
}
std::span<const float> FrameProcessor::last_phase(std::size_t ch) const {
    return channel_data.at(ch).last_phase;
}
std::span<const float> FrameProcessor::overlap(std::size_t ch) const {
    return { channel_data.at(ch).overlap.data() + spectral_size,
        spectral_size };
}
std::span<const float> FrameProcessor::analysis_spectrum(std::size_t ch) const {
    return { channel_data.at(ch).spectrum.data(), spectral_size };
}
std::span<const float> FrameProcessor::reconstructed_spectrum(
        std::size_t ch) const {
    return { channel_data.at(ch).reconstructed.data(), spectral_size };
}

} // namespace ekphore::irc::pv
#endif

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <stdexcept>

namespace ekphore::irc::pv {
namespace {
void irc_pv_phase_state_require(bool value) {
    if (!value)
        throw std::invalid_argument("invalid IRC PV spectral state view");
}
std::size_t irc_pv_phase_state_bins(std::size_t N) {
    // 128 only works for these loops
    irc_pv_phase_state_require(
            N >= 128 && N <= 32768 && std::has_single_bit(N));
    return N / 2;
}
void irc_pv_phase_state_finite_plane(
        std::span<const float> values, std::size_t H) {
    irc_pv_phase_state_require(values.size() == H);
    irc_pv_phase_state_require(std::ranges::all_of(
            values, [](float v) { return std::isfinite(v); }));
}
void irc_pv_phase_state_valid_peaks(std::span<const int> peaks, std::size_t H) {
    irc_pv_phase_state_require(peaks.size() <= H);
    irc_pv_phase_state_require(std::ranges::all_of(
            peaks, [H](int p) { return p >= 0 && std::size_t(p) < H; }));
}
struct irc_pv_phase_state_Memory {
    std::uintptr_t first, end;
};
template <class T>
irc_pv_phase_state_Memory irc_pv_phase_state_memory(std::span<T> s) {
    const auto first = reinterpret_cast<std::uintptr_t>(s.data());
    return { first, first + s.size_bytes() };
}
template <std::size_t I, std::size_t O>
void irc_pv_phase_state_disjoint(
        const std::array<irc_pv_phase_state_Memory, I> &inputs,
        const std::array<irc_pv_phase_state_Memory, O> &outputs) {
    const auto overlap = [](irc_pv_phase_state_Memory a,
                                 irc_pv_phase_state_Memory b) {
        return a.first < b.end && b.first < a.end;
    };
    for (std::size_t i = 0; i < O; ++i) {
        for (auto input : inputs)
            irc_pv_phase_state_require(!overlap(input, outputs[i]));
        for (std::size_t j = 0; j < i; ++j)
            irc_pv_phase_state_require(!overlap(outputs[j], outputs[i]));
    }
}
} // namespace
void process_transients(std::size_t K, std::size_t N, std::size_t analysisHop,
        float currentStretch, float &previousStretch,
        const TransientInput &input, const TransientOutput &output) {
    const auto H = irc_pv_phase_state_bins(N);
    irc_pv_phase_state_require(K >= 2 && K % 2 == 0 &&
                               K <= std::size_t(INT32_MAX) && analysisHop > 0 &&
                               analysisHop <= K);
    irc_pv_phase_state_require(
            std::isfinite(currentStretch) && currentStretch > 0 &&
            std::isfinite(previousStretch) && previousStretch > 0);
    irc_pv_phase_state_valid_peaks(input.peaks, H);
    irc_pv_phase_state_require(
            input.map.size() == H && std::ranges::all_of(input.map, [H](int p) {
                return p >= 0 && std::size_t(p) < H;
            }));
    irc_pv_phase_state_finite_plane(input.magnitude, H);
    irc_pv_phase_state_finite_plane(input.quarter_hop_history, H);
    irc_pv_phase_state_finite_plane(input.regular_history, H);
    irc_pv_phase_state_require(output.flags.size() == H &&
                               output.ratio.size() == H &&
                               output.work.size() == H);
    irc_pv_phase_state_disjoint(
            std::array{ irc_pv_phase_state_memory(input.map),
                    irc_pv_phase_state_memory(input.peaks),
                    irc_pv_phase_state_memory(input.magnitude),
                    irc_pv_phase_state_memory(input.quarter_hop_history),
                    irc_pv_phase_state_memory(input.regular_history),
                    irc_pv_phase_state_memory(
                            std::span<float>(&previousStretch, 1)) },
            std::array{ irc_pv_phase_state_memory(output.flags),
                    irc_pv_phase_state_memory(output.ratio),
                    irc_pv_phase_state_memory(output.work) });
    irc_pv_phase_state_disjoint(
            std::array{ irc_pv_phase_state_memory(input.map),
                    irc_pv_phase_state_memory(input.peaks),
                    irc_pv_phase_state_memory(input.magnitude),
                    irc_pv_phase_state_memory(input.quarter_hop_history),
                    irc_pv_phase_state_memory(input.regular_history) },
            std::array{ irc_pv_phase_state_memory(
                    std::span<float>(&previousStretch, 1)) });
    // this one stays even with a zero count
    output.flags[0] = previousStretch;
    const auto reference = previousStretch > 1.F && analysisHop == K / 4
                                   ? input.quarter_hop_history
                                   : input.regular_history;
    constexpr double threshold = std::bit_cast<double>(0x3e7ad7f29abcaf48ULL);
    constexpr float epsilon = std::bit_cast<float>(0x179abe15U);
    for (std::size_t r = 0; r < input.peaks.size(); ++r) {
        output.ratio[r] = 0.F;
        output.work[r] = 0;
        float denominator = 0.F;
        std::size_t k = std::size_t(input.peaks[r ? r - 1 : 0]);
        for (; k < H && input.map[k] <= input.peaks[r]; ++k) {
            volatile float difference = input.magnitude[k] - reference[k];
            // float abs difference, then compare in double
            if (double(std::abs(float(difference))) <= threshold)
                continue;
            volatile float reference_square = reference[k] * reference[k];
            volatile float denominator_added = denominator + reference_square;
            denominator = denominator_added;
            volatile float difference_square = difference * difference;
            volatile float numerator_added =
                    difference_square + output.ratio[r];
            output.ratio[r] = numerator_added;
        }
        volatile float adjusted = denominator + epsilon;
        volatile float ratio = output.ratio[r] / adjusted;
        output.ratio[r] = ratio;
    }
    for (std::size_t r = 0; r < input.peaks.size(); ++r)
        output.flags[r] = output.ratio[r] > 1.F ? 1.F : -1.F;
    previousStretch = currentStretch; // even with an empty peak list
}
void synchronize_channels(
        std::size_t N, const SyncInput &input, const SyncOutput &output) {
    const auto H = irc_pv_phase_state_bins(N);
    irc_pv_phase_state_valid_peaks(input.peaks, H);
    irc_pv_phase_state_finite_plane(input.channel0_retained_synthesis, H);
    irc_pv_phase_state_finite_plane(input.current_phase, H);
    irc_pv_phase_state_finite_plane(input.channel0_previous_phase, H);
    irc_pv_phase_state_require(output.synthesis_phase.size() == H &&
                               output.phase_offset.size() == H &&
                               output.cosine.size() == H &&
                               output.sine.size() == H);
    irc_pv_phase_state_disjoint(
            std::array{ irc_pv_phase_state_memory(input.peaks),
                    irc_pv_phase_state_memory(
                            input.channel0_retained_synthesis),
                    irc_pv_phase_state_memory(input.current_phase),
                    irc_pv_phase_state_memory(input.channel0_previous_phase) },
            std::array{ irc_pv_phase_state_memory(output.synthesis_phase),
                    irc_pv_phase_state_memory(output.phase_offset),
                    irc_pv_phase_state_memory(output.cosine),
                    irc_pv_phase_state_memory(output.sine) });
    for (const int peak : input.peaks) {
        const auto p = std::size_t(peak);
        volatile float delta =
                input.current_phase[p] - input.channel0_previous_phase[p];
        volatile float synthesis = input.channel0_retained_synthesis[p] + delta;
        output.synthesis_phase[p] = synthesis;
        volatile float offset = synthesis - input.current_phase[p];
        output.phase_offset[p] = offset;
        output.cosine[p] = std::cos(float(offset));
        output.sine[p] = std::sin(output.phase_offset[p]);
    }
}
} // namespace ekphore::irc::pv

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <stdexcept>

namespace ekphore::irc::pv {
namespace {
std::size_t irc_pv_regions_geometry(std::size_t n) {
    // 512 and up only, we don't know what smaller ones do
    if (n < 512 || !std::has_single_bit(n) || n > 32768)
        throw std::invalid_argument("unqualified IRC PV region geometry");
    return n;
}
} // namespace
Regions::Regions(std::size_t n)
    : spectral_size(irc_pv_regions_geometry(n)), bins(spectral_size / 2),
      bin_map(bins + 1), candidate(bins + 1), scratch(bins + 1) {}

std::size_t Regions::select(std::span<const float> magnitude,
        std::span<const float> frequency, unsigned analysis_hop) {
    return select(magnitude, frequency, analysis_hop, bins);
}

void Regions::reset_tables() noexcept {
    // clear the bins, but keep the selected count and extra sentinel entries
    std::fill_n(bin_map.begin(), bins, 0);
    std::fill_n(candidate.begin(), bins, 0);
    std::fill_n(scratch.begin(), bins, 0);
}

std::size_t Regions::select(std::span<const float> magnitude,
        std::span<const float> frequency, unsigned analysis_hop,
        std::size_t usable_bins) {
    if (magnitude.size() != bins || frequency.size() != bins || !analysis_hop ||
            analysis_hop > spectral_size || usable_bins == 0 ||
            usable_bins > bins ||
            !std::ranges::all_of(
                    magnitude, [](float v) { return std::isfinite(v); }) ||
            !std::ranges::all_of(
                    frequency, [](float v) { return std::isfinite(v); }))
        throw std::invalid_argument("invalid IRC PV region input");
    const float factor =
            float(spectral_size) /
            (float(analysis_hop) * std::bit_cast<float>(0x40c90fdbU));
    // check BEFORE touching the tables
    for (float value : frequency.first(usable_bins)) {
        const float raw = std::floor(factor * value + .5F);
        if (raw < -0x1p31F || raw >= 0x1p31F)
            throw std::invalid_argument(
                    "IRC PV frequency index outside finite signed range");
    }
    for (std::size_t j = 0; j < usable_bins; ++j) {
        const int raw = int(std::floor(factor * frequency[j] + .5F));
        bin_map[j] = raw <= 0 ? int(j) : std::min(raw, int(bins) - 1);
    }
    scratch[0] = 1;
    const int threshold = int(spectral_size >> 8);
    int p = 0;
    // don't clear the unused entries, 512 still reads one
    for (int j = 1; j < std::max(threshold, 2); ++j)
        candidate[p++] = j;
    for (int j = threshold; j < int(usable_bins); ++j) {
        scratch[j] = bin_map[j] - j;
        if (scratch[j] <= 0 && scratch[j - 1] > 0)
            candidate[p++] = j;
    }
    const auto mag = [&](int i) { return magnitude[std::size_t(i)]; };
    const auto distance = [&](int index, int peak) {
        return std::abs(bin_map.at(index) - peak);
    };
    int refined = 0;
    if (p > 0) {
        while (candidate.at(refined) < int(spectral_size >> 6)) {
            scratch[refined] = candidate[refined];
            ++refined;
            const int next = candidate.at(refined); // one past, on purpose
            if (refined >= p || next >= int(spectral_size >> 6))
                break;
        }
        int scan = refined;
        if (refined < 2) {
            if (mag(candidate[2]) <= mag(candidate[1]) ||
                    mag(candidate[0]) > mag(candidate[1]))
                scratch[refined++] = candidate[1];
            scan = 2;
        }
        int current = candidate.at(scan);
        const int previous = candidate.at(scan - 1);
        int boundary = std::max(current, 1);
        while (distance(boundary, current) < distance(boundary, previous)) {
            if (boundary >= 2)
                --boundary;
            else {
                boundary = 0;
                break;
            }
        }
        const int lower_limit =
                std::min(int(spectral_size / 8), int(usable_bins));
        while (scan < p && current < lower_limit) {
            const int left = boundary + 1;
            const int peak = std::max(current, 1);
            const int next = candidate.at(++scan);
            boundary = peak;
            while (boundary < int(bins) - 1 &&
                    distance(boundary, current) < distance(boundary, next)) {
                const int old = boundary++;
                if (old >= int(usable_bins) - 2)
                    break;
            }
            const int right = boundary > int(usable_bins) - 2
                                      ? int(usable_bins) - 2
                                      : boundary - 1;
            if (mag(peak) >= mag(left) && mag(peak) >= mag(right))
                scratch[refined++] = peak;
            // the next left is this right + 1
            boundary = right;
            current = next;
        }
        scan = std::max(scan, 1);
        if (scan < p) {
            current = candidate.at(scan);
            while (scan < p && current < int(usable_bins)) {
                const int next = candidate.at(scan + 1);
                if (mag(current) >= mag(next) &&
                        mag(current) >= mag(candidate.at(scan - 1)))
                    scratch[refined++] = current;
                ++scan;
                current = next;
            }
        }
        scratch[std::max(refined, 1) - 1] = int(usable_bins) - 1;
    }
    if (refined > 0) {
        std::copy_n(scratch.begin(), refined, candidate.begin());
        count = refined;
    } else {
        count = std::max(p, 1);
        candidate[count - 1] = int(usable_bins) - 1;
    }
    int cursor = 1;
    for (std::size_t j = 0; j < usable_bins; ++j) {
        if (cursor > int(count))
            continue;
        int k = cursor;
        while (k <= int(count)) {
            const int previous = candidate.at(k - 1), next = candidate.at(k);
            if (k == int(count) || next == 0 ||
                    std::abs(bin_map[j] - previous) <=
                            std::abs(bin_map[j] - next)) {
                bin_map[j] = previous;
                cursor = std::max(1, k - 1);
                break;
            }
            ++k;
        }
    }
    return count;
}
} // namespace ekphore::irc::pv

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace ekphore::irc::pv {
namespace {
std::int32_t irc_pv_resample_checked_cursor(double phase) {
    if (!std::isfinite(phase) || phase < 0.0 ||
            phase >= double(std::numeric_limits<std::int32_t>::max()) + 1.0)
        throw std::overflow_error(
                "IRC resample phase outside signed cursor domain");
    return static_cast<std::int32_t>(phase);
}
} // namespace

FastResample::FastResample(std::size_t channels, std::size_t input_capacity)
    : buffer_capacity(input_capacity) {
    if (channels == 0 || buffer_capacity == 0 ||
            buffer_capacity >
                    std::size_t(std::numeric_limits<std::int32_t>::max()))
        throw std::invalid_argument("IRC resample dimensions");
    planes.assign(channels, std::vector<float>(buffer_capacity + 4, 0.F));
}

void FastResample::reset() noexcept {
    resample_phase = 0.0;
    resample_fraction = 0.F;
    read_cursor = 0;
    // only exposed-4 and exposed-12, NOT the whole thing
    for (auto &plane : planes)
        std::fill(plane.begin() + 1, plane.begin() + 5, 0.F);
}

std::size_t FastResample::output_count(
        std::size_t n, float ratio, std::size_t limit) const {
    if (n > buffer_capacity || !std::isfinite(ratio) || ratio <= 0.F)
        throw std::invalid_argument(
                "IRC resample count or positive finite ratio");
    double phase = resample_phase;
    std::int32_t cursor = read_cursor;
    std::size_t count = 0;
    while (cursor < static_cast<std::int32_t>(n)) {
        if (count == limit)
            throw std::length_error("IRC resample output capacity");
        ++count;
        const double next = phase + double(ratio);
        if (next <= phase)
            throw std::overflow_error("IRC resample phase cannot advance");
        phase = next;
        cursor = irc_pv_resample_checked_cursor(phase);
    }
    // just a sanity check
    irc_pv_resample_checked_cursor(phase - double(n));
    return count;
}

std::size_t FastResample::process(std::span<const std::span<const float>> input,
        std::size_t n, std::span<const std::span<float>> output, float ratio) {
    if (input.size() != planes.size() || output.size() != planes.size())
        throw std::invalid_argument("IRC resample plane count");
    std::size_t available = std::numeric_limits<std::size_t>::max();
    for (std::size_t ch = 0; ch < planes.size(); ++ch) {
        if (input[ch].size() < n)
            throw std::invalid_argument("IRC resample input extent");
        available = std::min(available, output[ch].size());
    }
    const auto count = output_count(n, ratio, available);
    // outputs can't overlap, check BEFORE touching anything
    if (count != 0) {
        for (std::size_t ch = 0; ch < output.size(); ++ch) {
            const auto begin =
                    reinterpret_cast<std::uintptr_t>(output[ch].data());
            for (std::size_t other = 0; other < ch; ++other) {
                const auto previous =
                        reinterpret_cast<std::uintptr_t>(output[other].data());
                const auto distance =
                        begin > previous ? begin - previous : previous - begin;
                if (distance < count * sizeof(float))
                    throw std::invalid_argument(
                            "IRC resample overlapping output planes");
            }
        }
    }
    // copy every input lane before writing any output, since they may alias
    for (std::size_t ch = 0; ch < planes.size(); ++ch)
        std::copy_n(input[ch].begin(), n, planes[ch].begin() + 4);

    double phase = resample_phase;
    auto cursor = read_cursor;
    float t = resample_fraction;
    for (std::size_t frame = 0; frame < count; ++frame) {
        // keep each float operation separate, without FMA
        const float t2 = t * t;
        const float t3 = t * t2;
        const float twice_t3 = t3 + t3;
        const float three_t2 = t2 * 3.F;
        const float w0 = (twice_t3 - three_t2) + 1.F;
        const float w1 = t + (t3 - (t2 + t2));
        const float w2 = t3 - t2;
        const float w3 = three_t2 - twice_t3;
        for (std::size_t ch = 0; ch < planes.size(); ++ch) {
            const auto *p = planes[ch].data() + 4;
            const float h0 = p[cursor - 3], h1 = p[cursor - 2];
            const float h2 = p[cursor - 1], h3 = p[cursor];
            // keep the cancellation; reducing to differences changes rounding
            const float m1 = (((h1 - h0) + h2) - h1) * .5F;
            const float m2 = (((h2 - h1) + h3) - h2) * .5F;
            const float a = w0 * h1;
            const float b = m1 * w1;
            const float c = m2 * w2;
            const float d = w3 * h2;
            output[ch][frame] = ((a + b) + c) + d;
        }
        phase += double(ratio);
        cursor = static_cast<std::int32_t>(phase); // checked above
        t = float(phase - double(cursor));
    }
    resample_fraction = t; // NOT recomputed after subtracting N
    resample_phase = phase - double(n);
    read_cursor = static_cast<std::int32_t>(resample_phase);
    // copy in this order, including when fewer than three samples were consumed
    for (auto &storage : planes) {
        auto *p = storage.data() + 4;
        const auto end = static_cast<std::ptrdiff_t>(n);
        p[-3] = p[end - 3];
        p[-2] = p[end - 2];
        p[-1] = p[end - 1];
    }
    return count;
}

std::array<float, 3> FastResample::history(std::size_t channel) const {
    const auto &p = planes.at(channel);
    return { p[1], p[2], p[3] };
}
} // namespace ekphore::irc::pv

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#if defined(__APPLE__)

#endif

namespace ekphore::irc::pv {
namespace {
void irc_pv_stream_require(bool valid) {
    if (!valid)
        throw std::invalid_argument("unqualified IRC PV stream input");
}
unsigned irc_pv_stream_window(float rate) {
    irc_pv_stream_require(std::isfinite(rate) && rate > 0);
    return rate < 32000.F    ? 2048
           : rate < 64000.F  ? 4096
           : rate < 128000.F ? 8192
           : rate < 256000.F ? 16384
                             : 32768;
}
int irc_pv_stream_integer(double value) {
    irc_pv_stream_require(std::isfinite(value) && value >= double(INT32_MIN) &&
                          value < double(INT32_MAX));
    return int(value);
}
float irc_pv_stream_fm(float a, float b) {
    volatile float r = a * b;
    return r;
}
float irc_pv_stream_fa(float a, float b) {
    volatile float r = a + b;
    return r;
}
float irc_pv_stream_fs(float a, float b) {
    volatile float r = a - b;
    return r;
}
float irc_pv_stream_fd(float a, float b) {
    volatile float r = a / b;
    return r;
}
double irc_pv_stream_dm(double a, double b) {
    volatile double r = a * b;
    return r;
}
double irc_pv_stream_da(double a, double b) {
    volatile double r = a + b;
    return r;
}
double irc_pv_stream_ds(double a, double b) {
    volatile double r = a - b;
    return r;
}
double irc_pv_stream_dd(double a, double b) {
    volatile double r = a / b;
    return r;
}
} // namespace
#if defined(__APPLE__)
FrameStreamBackend::FrameStreamBackend() = default;
FrameStreamBackend::~FrameStreamBackend() = default;
void FrameStreamBackend::initialize(std::size_t K, std::size_t channels,
        unsigned out, unsigned analysis, unsigned norm, float stretch) {
    if (frame_processor)
        throw std::logic_error("IRC frame backend cannot be reinitialized");
    frame_processor = std::make_unique<FrameProcessor>(
            K, analysis, out, norm, channels, stretch);
}
FrameProcessor &FrameStreamBackend::require_frame() {
    if (!frame_processor)
        throw std::logic_error("IRC frame backend not initialized");
    return *frame_processor;
}
const FrameProcessor &FrameStreamBackend::frame() const {
    if (!frame_processor)
        throw std::logic_error("IRC frame backend not initialized");
    return *frame_processor;
}
void FrameStreamBackend::set_hops(
        unsigned out, unsigned analysis, unsigned norm, float stretch) {
    require_frame().set_hops(out, analysis, norm, stretch);
}
void FrameStreamBackend::set_cutoff(unsigned bin) {
    require_frame().set_cutoff(bin);
}
void FrameStreamBackend::reset() { require_frame().reset(); }
unsigned FrameStreamBackend::process_stage(
        std::span<const std::span<const float>> input,
        std::span<const std::span<float>> output, int stage) {
    auto &f = require_frame();
    if (input.size() != f.channels() || output.size() != f.channels())
        throw std::invalid_argument("IRC frame backend plane count");
    if (stage < 0) {
        const auto channel = std::size_t(-std::int64_t(stage) - 1);
        if (channel >= f.channels() || input[channel].size() < f.analysis_hop())
            throw std::invalid_argument("IRC negative stage/channel");
        return f.prime_channel(channel, input[channel].first(f.analysis_hop()));
    }
    const auto channel = std::size_t(stage / 5);
    const auto substage = unsigned(stage % 5);
    if (channel >= f.channels())
        throw std::invalid_argument("IRC positive stage/channel");
    std::span<const float> in;
    if (substage == 0) {
        if (input[channel].size() < f.analysis_hop())
            throw std::invalid_argument("IRC frame input extent");
        in = input[channel].first(f.analysis_hop());
    }
    f.run_stage(channel, substage, in, output[channel]);
    return f.output_hop();
}
#endif
ElastiqueStream::ElastiqueStream(float rate, unsigned channels,
        unsigned quantum, StreamFrameBackend &backend, StreamMode mode)
    : configured_sample_rate(rate), channel_count(channels),
      output_quantum(quantum), K(irc_pv_stream_window(rate)),
      buffer_capacity(K * 16), stream_mode(mode), frame_backend(backend),
      resampler(channels, 16384) {
    irc_pv_stream_require(
            (channels == 1 || channels == 2) && quantum > 0 && quantum <= K);
    irc_pv_stream_require(
            mode == StreamMode::standard || mode == StreamMode::pro);
    input_buffer.assign(channels, std::vector<float>(buffer_capacity));
    // K*16 of zeros in front
    output_buffer.assign(channels, std::vector<float>(buffer_capacity * 2));
    const unsigned initial_hop = K / (stream_mode == StreamMode::pro ? 8 : 4);
    stream_state.analysis_hop = initial_hop;
    stream_state.output_hop = initial_hop;
    stream_state.frame_output = K;
    stream_state.startup = true;
    stream_state.cutoff_cap = K / 2;
    frame_backend.initialize(
            K, channel_count, initial_hop, initial_hop, initial_hop, 100.F);
    float unity = 1.F;
    set_parameters(unity, 1.F);
    reset();
}
void ElastiqueStream::reset() {
    stream_state.buffered = stream_state.output_position =
            stream_state.drain_generated = 0;
    stream_state.initial_position = stream_state.alignment = 0;
    stream_state.input_position = 0;
    stream_state.startup = true;
    stream_state.forced_resample = false;
    stream_state.requested_pitch = stream_state.current_pitch = 1.F;
    stream_state.reciprocal = stream_state.previous_reciprocal = 1.;
    stream_state.nominal_total = stream_state.actual_total =
            stream_state.nominal_position = stream_state.resample_error = 0.;
    stream_state.drift_bound = float(K / 2);
    resampler.reset();
    frame_backend.reset();
}
void ElastiqueStream::set_parameters(
        float &stretch, float pitch, bool preserve_position) {
    irc_pv_stream_require(std::isfinite(stretch) && std::isfinite(pitch) &&
                          stretch > 0 && pitch > 0);
    const float combined = irc_pv_stream_fm(stretch, pitch);
    irc_pv_stream_require(combined >= .1F && combined <= 10.F);
    volatile float exponent = std::floor(
            irc_pv_stream_fm(std::log(irc_pv_stream_fa(float(K), 1.F)),
                    std::bit_cast<float>(0x3fb8aa3bU)));
    const int power = irc_pv_stream_integer(std::exp2(float(exponent)));
    unsigned shift = combined <= 1.5F          ? 2
                     : double(combined) <= 2.2 ? 3
                     : double(combined) <= 3.2 ? 4
                     : double(combined) <= 4.2 ? 5
                                               : 6;
    // at least 3
    if (stream_mode == StreamMode::pro)
        shift = std::max(shift, 3U);
    const unsigned A = unsigned(power) >> shift;
    const unsigned S = unsigned(irc_pv_stream_integer(
                               irc_pv_stream_fm(combined, float(A)))) &
                       ~1U;
    irc_pv_stream_require(A > 0 && S > 0 && S <= 16384);
    const float effective = irc_pv_stream_fd(float(S), float(A));
    const float reciprocal_pitch = irc_pv_stream_fd(1.F, pitch);
    const int start = preserve_position
                              ? -int(K / 2)
                              : irc_pv_stream_integer(
                                        irc_pv_stream_fd(-float(K / 2), pitch));
    irc_pv_stream_require(start >= -int(buffer_capacity));
    stream_state.requested_pitch = pitch;
    stream_state.combined_stretch = effective;
    stream_state.reciprocal = double(irc_pv_stream_fd(1.F, effective));
    stretch = irc_pv_stream_fm(reciprocal_pitch, effective);
    if (stream_state.startup) {
        stream_state.output_position = start;
        stream_state.initial_position = -int(K / 2);
        stream_state.input_position = -double(K / 2);
        stream_state.alignment = 0;
        stream_state.frame_output = S;
    }
    frame_backend.set_hops(S, A, S, effective);
    // update this even after startup, or process/query would use the previous S
    // while the frame processor already uses the new hop
    stream_state.frame_output = S;
    stream_state.analysis_hop = A;
    stream_state.output_hop = S;
    stream_state.nominal_position = stream_state.nominal_total;
    if (preserve_position)
        stream_state.nominal_position = irc_pv_stream_ds(
                stream_state.nominal_position,
                irc_pv_stream_ds(
                        irc_pv_stream_dm(irc_pv_stream_dd(1., double(pitch)),
                                double(K / 2)),
                        double(K / 2)));
    stream_state.forced_resample = true;
}
void ElastiqueStream::update_pitch(bool query) {
    float pitch = stream_state.requested_pitch;
    if (irc_pv_stream_integer(irc_pv_stream_ds(stream_state.actual_total,
                stream_state.nominal_position)) != 0) {
        const double delta =
                irc_pv_stream_ds(irc_pv_stream_da(stream_state.actual_total,
                                         stream_state.resample_error),
                        stream_state.nominal_position);
        const float bound = std::max(
                float(K / 2), float(irc_pv_stream_dm(std::abs(delta), 1.25)));
        stream_state.drift_bound = bound;
        pitch = float(irc_pv_stream_dm(
                irc_pv_stream_dd(
                        irc_pv_stream_da(delta, double(bound)), double(bound)),
                double(pitch)));
        if (query)
            pitch = std::max(pitch, stream_state.requested_pitch);
    }
    irc_pv_stream_require(std::isfinite(pitch) && pitch > 0);
    stream_state.current_pitch = pitch;
}
void ElastiqueStream::set_cutoff_frequency(float frequency) {
    irc_pv_stream_require(std::isfinite(frequency));
    // round UP, then clamp to [0,K/2]
    const auto bin = irc_pv_stream_integer(std::ceil(irc_pv_stream_fm(
            irc_pv_stream_fd(frequency, configured_sample_rate), float(K))));
    stream_state.cutoff_cap = unsigned(std::clamp(bin, 0, int(K / 2)));
    frame_backend.set_cutoff(stream_state.cutoff_cap);
}
void ElastiqueStream::set_output_offset(int position) {
    irc_pv_stream_require(position >= -int(buffer_capacity) &&
                          position < int(buffer_capacity));
    stream_state.output_position = position;
}
void ElastiqueStream::set_time_position(double position) {
    irc_pv_stream_require(std::isfinite(position));
    stream_state.input_position = position;
}
unsigned ElastiqueStream::frames_needed(unsigned quantum) {
    irc_pv_stream_require(quantum > 0 && quantum <= buffer_capacity);
    output_quantum = quantum; // BEFORE calculating
    return frames_needed();
}
std::int64_t ElastiqueStream::time_position() const {
    // signed, then truncate
    const double at = irc_pv_stream_ds(
            irc_pv_stream_da(stream_state.input_position,
                    double(stream_state.alignment)),
            irc_pv_stream_dm(irc_pv_stream_dm(stream_state.previous_reciprocal,
                                     double(stream_state.output_position)),
                    double(stream_state.current_pitch)));
    irc_pv_stream_require(std::isfinite(at) && at >= -0x1p63 && at < 0x1p63);
    return std::int64_t(at);
}
unsigned ElastiqueStream::frames_needed() {
    update_pitch(true);
    const int advance = irc_pv_stream_integer(std::floor(irc_pv_stream_fd(
            float(stream_state.frame_output), stream_state.current_pitch)));
    irc_pv_stream_require(advance > 0);
    int need = -stream_state.buffered, position = stream_state.output_position;
    while (position <= int(output_quantum)) {
        position = irc_pv_stream_integer(double(position) + advance);
        need = irc_pv_stream_integer(double(need) + stream_state.analysis_hop);
    }
    if (stream_state.startup)
        need = irc_pv_stream_integer(double(need) + K / 2);
    // round up, NOT truncate
    return unsigned(
            std::max(0, irc_pv_stream_integer(irc_pv_stream_fm(
                                std::ceil(irc_pv_stream_fd(float(need),
                                        float(stream_state.analysis_hop))),
                                float(stream_state.analysis_hop)))));
}
void ElastiqueStream::prepare_views() {
    irc_pv_stream_require(
            stream_state.output_position >= -int(buffer_capacity) &&
            stream_state.output_position < int(buffer_capacity));
    for (unsigned c = 0; c < channel_count; ++c) {
        input_views[c] = std::span<const float>(input_buffer[c])
                                 .first(stream_state.analysis_hop);
        const auto offset = std::size_t(
                int(buffer_capacity) + stream_state.output_position);
        output_views[c] = std::span<float>(output_buffer[c]).subspan(offset);
    }
}
void ElastiqueStream::consume_input() {
    irc_pv_stream_require(
            stream_state.buffered >= int(stream_state.analysis_hop));
    stream_state.buffered -= int(stream_state.analysis_hop);
    for (auto &plane : input_buffer)
        std::memmove(plane.data(), plane.data() + stream_state.analysis_hop,
                std::size_t(stream_state.buffered) * sizeof(float));
}
void ElastiqueStream::advance_drift() {
    const float nominal = irc_pv_stream_fd(
            float(stream_state.frame_output), stream_state.requested_pitch);
    update_pitch(false);
    const float actual = irc_pv_stream_fd(
            float(stream_state.frame_output), stream_state.current_pitch);
    stream_state.nominal_total =
            irc_pv_stream_da(stream_state.nominal_total, double(nominal));
    stream_state.nominal_position =
            irc_pv_stream_da(stream_state.nominal_position, double(nominal));
    stream_state.actual_total =
            irc_pv_stream_da(stream_state.actual_total, double(actual));
}
unsigned ElastiqueStream::hop(bool drain) {
    if (stream_state.previous_reciprocal != stream_state.reciprocal) {
        stream_state.alignment = irc_pv_stream_integer(irc_pv_stream_ds(
                double(K / 2),
                irc_pv_stream_dm(stream_state.reciprocal, double(K / 2))));
        stream_state.previous_reciprocal = stream_state.reciprocal;
    }
    prepare_views();
    if (drain) {
        for (auto &plane : input_buffer)
            std::fill_n(plane.begin(), stream_state.analysis_hop, 0.F);
        const float nyquist = irc_pv_stream_fm(configured_sample_rate, .5F);
        const float cutoff = std::min(
                nyquist, irc_pv_stream_fd(nyquist, stream_state.current_pitch));
        stream_state.cutoff_cap = unsigned(std::clamp(
                irc_pv_stream_integer(std::ceil(irc_pv_stream_fm(
                        irc_pv_stream_fd(cutoff, configured_sample_rate),
                        float(K)))),
                0, int(K / 2)));
        frame_backend.set_cutoff(stream_state.cutoff_cap);
    } else {
        if (stream_state.startup) {
            const unsigned warmup = K / stream_state.analysis_hop;
            if (warmup >= 4)
                for (unsigned i = 0; i < warmup / 2 - 1; ++i) {
                    for (unsigned c = 0; c < channel_count; ++c)
                        stream_state.frame_output = frame_backend.process_stage(
                                { input_views.data(), channel_count },
                                { output_views.data(), channel_count },
                                -int(c) - 1);
                    consume_input();
                }
            stream_state.startup = false;
        }
        advance_drift();
        // only standard mode updates the cutoff while processing input;
        // draining updates it in both modes above
        if (stream_mode == StreamMode::standard) {
            const auto cutoff =
                    irc_pv_stream_integer(std::ceil(irc_pv_stream_fm(
                            irc_pv_stream_fd(.5F, stream_state.current_pitch),
                            float(K))));
            irc_pv_stream_require(cutoff >= 0);
            frame_backend.set_cutoff(
                    std::min(stream_state.cutoff_cap, unsigned(cutoff)));
        }
    }
    // only warmup cares about the count
    for (unsigned stage = 0; stage < channel_count * 5; ++stage)
        (void)frame_backend.process_stage({ input_views.data(), channel_count },
                { output_views.data(), channel_count }, int(stage));
    if (drain)
        advance_drift(); // AFTER the PV
    unsigned count = stream_state.frame_output;
    if (stream_state.current_pitch != 1.F || stream_state.forced_resample) {
        std::array<std::span<const float>, 2> resample_input;
        for (unsigned c = 0; c < channel_count; ++c)
            resample_input[c] =
                    output_views[c].first(stream_state.frame_output);
        count = unsigned(
                resampler.process({ resample_input.data(), channel_count },
                        stream_state.frame_output,
                        { output_views.data(), channel_count },
                        stream_state.current_pitch));
    }
    const float error = irc_pv_stream_fs(
            float(count), irc_pv_stream_fd(float(stream_state.frame_output),
                                  stream_state.current_pitch));
    stream_state.resample_error =
            irc_pv_stream_da(stream_state.resample_error, double(error));
    if (!drain)
        consume_input();
    stream_state.output_position =
            irc_pv_stream_integer(double(stream_state.output_position) + count);
    stream_state.input_position = irc_pv_stream_da(
            stream_state.input_position, double(stream_state.analysis_hop));
    return count;
}
void ElastiqueStream::flush(std::span<const std::span<float>> output) {
    stream_state.output_position -= int(output_quantum);
    for (unsigned i = 0; i < output_quantum; ++i) {
        const float a = output_buffer[0][buffer_capacity + i];
        if (channel_count == 1)
            output[0][i] = a;
        else {
            const float b = output_buffer[1][buffer_capacity + i];
            output[0][i] = irc_pv_stream_fd(
                    irc_pv_stream_fa(a, b), std::bit_cast<float>(0x3f99999aU));
            output[1][i] = irc_pv_stream_fd(
                    irc_pv_stream_fs(a, b), std::bit_cast<float>(0x3f4ccccdU));
        }
    }
    for (auto &plane : output_buffer)
        std::memmove(plane.data() + buffer_capacity,
                plane.data() + buffer_capacity + output_quantum,
                std::size_t(stream_state.output_position) * sizeof(float));
}
int ElastiqueStream::process(
        const std::span<const std::span<const float>> *input, unsigned count,
        std::span<const std::span<float>> output) {
    irc_pv_stream_require(output.size() == channel_count);
    for (auto plane : output)
        irc_pv_stream_require(plane.size() >= output_quantum);
    if (count) {
        irc_pv_stream_require(
                input && input->size() == channel_count &&
                std::size_t(stream_state.buffered) + count <= buffer_capacity);
        for (auto plane : *input)
            irc_pv_stream_require(
                    plane.size() >= count &&
                    std::ranges::all_of(plane.first(count),
                            [](float x) { return std::isfinite(x); }));
        for (unsigned i = 0; i < count; ++i) {
            const auto index = std::size_t(stream_state.buffered) + i;
            if (channel_count == 1)
                input_buffer[0][index] = (*input)[0][i];
            else {
                const float left = irc_pv_stream_fm(
                        (*input)[0][i], std::bit_cast<float>(0x3f19999aU));
                const float right = irc_pv_stream_fm(
                        (*input)[1][i], std::bit_cast<float>(0x3ecccccdU));
                input_buffer[0][index] = irc_pv_stream_fa(left, right);
                input_buffer[1][index] = irc_pv_stream_fs(left, right);
            }
        }
        stream_state.buffered += int(count);
    }
    if (stream_state.buffered >= int(stream_state.analysis_hop)) {
        while (stream_state.output_position < int(output_quantum) &&
                stream_state.buffered >= int(stream_state.analysis_hop))
            hop(false);
    } else if (!input) {
        const int target = irc_pv_stream_integer(
                irc_pv_stream_fs(float(-stream_state.initial_position),
                        irc_pv_stream_fm(stream_state.combined_stretch,
                                float(stream_state.initial_position))));
        if (stream_state.drain_generated < target) {
            const auto count_generated = hop(true);
            stream_state.drain_generated = irc_pv_stream_integer(
                    double(stream_state.drain_generated) + count_generated);
            if (stream_state.drain_generated > target)
                stream_state.output_position -=
                        stream_state.drain_generated - target;
        }
    }
    if (stream_state.output_position < int(output_quantum))
        return -1;
    flush(output);
    return 0;
}
std::vector<std::vector<float>> ElastiqueStream::apply_offline(
        std::span<const std::span<const float>> source, float stretch,
        float pitch, std::size_t capacity) {
    irc_pv_stream_require(source.size() == channel_count && !source.empty());
    const auto frames = source[0].size();
    for (auto plane : source)
        irc_pv_stream_require(plane.size() == frames);
    irc_pv_stream_require(capacity >= output_quantum);
    std::vector<std::vector<float>> destination(
            channel_count, std::vector<float>(capacity));
    set_parameters(stretch, pitch);
    unsigned need = frames_needed();
    std::size_t cursor = 0, block = 0;
    std::array<std::span<const float>, 2> in;
    std::array<std::span<float>, 2> out;
    const auto bind_output = [&] {
        irc_pv_stream_require(block <= capacity / output_quantum &&
                              (block + 1) * output_quantum <= capacity);
        for (unsigned c = 0; c < channel_count; ++c)
            out[c] = std::span<float>(destination[c])
                             .subspan(block * output_quantum, output_quantum);
    };
    while (cursor + need < frames) {
        for (unsigned c = 0; c < channel_count; ++c)
            in[c] = source[c].subspan(cursor, need);
        bind_output();
        const std::span<const std::span<const float>> input(
                in.data(), channel_count);
        (void)process(&input, need, { out.data(), channel_count });
        cursor += need;
        ++block;
        need = frames_needed();
    }
    irc_pv_stream_require(need >= frames - cursor);
    std::vector<std::vector<float>> final(
            channel_count, std::vector<float>(need));
    for (unsigned c = 0; c < channel_count; ++c) {
        std::copy(source[c].begin() + std::ptrdiff_t(cursor), source[c].end(),
                final[c].begin());
        in[c] = final[c];
    }
    bind_output();
    const std::span<const std::span<const float>> input(
            in.data(), channel_count);
    (void)process(&input, need, { out.data(), channel_count });
    int status;
    do {
        ++block;
        bind_output();
        status = process(nullptr, 0, { out.data(), channel_count });
    } while (status != -1);
    reset();
    return destination;
}
} // namespace ekphore::irc::pv
