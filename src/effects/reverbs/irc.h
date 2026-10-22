// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "irc_staged.h"
#include <array>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>
namespace ekphore::irc {
class StagedPairedConvolver;
namespace diagnostic {
struct ComparisonState;
}

struct Envelope {
    bool active{};
    std::array<float, 8> times{};  // normalized merged-response positions
    std::array<float, 8> levels{}; // dB, not linear amplitude
    bool operator==(const Envelope &) const = default;
};

// in place, BEFORE auto gain and predelay. throws on bad input before touching
// anything
void apply_envelope(
        std::span<std::vector<float>> channels, const Envelope &envelope);

[[nodiscard]] std::size_t pre_delay_frames(
        float milliseconds, std::uint32_t processing_rate);

[[nodiscard]] float auto_gain(std::span<const std::vector<float>> channels);

[[nodiscard]] std::vector<std::vector<float>> prepare_low_latency_ir(
        std::span<const std::vector<float>> channels,
        std::size_t convolution_block_size, bool apply_auto_gain,
        std::size_t pre_delay_frames = 0);

class PairedConvolver {
  public:
    enum class Backend { build_default, legacy, staged_ordinary };
    [[nodiscard]] static Backend default_backend() noexcept;
    PairedConvolver();
    ~PairedConvolver();
    PairedConvolver(const PairedConvolver &);
    PairedConvolver &operator=(const PairedConvolver &);
    PairedConvolver(PairedConvolver &&) noexcept;
    PairedConvolver &operator=(PairedConvolver &&) noexcept;
    // allocates, NEVER from audio
    [[nodiscard]] static PairedConvolver prepare(
            std::vector<std::vector<float>> impulse_channels,
            unsigned rate_divisor = 1,
            Backend backend = Backend::build_default);
    // nothing can be processing either one meanwhile, and free the old one off
    // audio
    void swap(PairedConvolver &other) noexcept;
    void configure(std::vector<std::vector<float>> impulse_channels);
    void reset();
    [[nodiscard]] std::pair<float, float> process(float left, float right);
    [[nodiscard]] bool configured() const noexcept;
    // in engine frames, NOT processing rate frames
    [[nodiscard]] std::size_t impulse_frames() const noexcept;
    [[nodiscard]] unsigned rate_divisor() const noexcept;
    [[nodiscard]] Backend backend() const noexcept;

  private:
    PairedConvolutionRate convolution_rate;
    std::unique_ptr<StagedPairedConvolver> staged;
    std::unique_ptr<diagnostic::ComparisonState> comparison;
    [[nodiscard]] std::pair<float, float> process_core(float left, float right);
    // the head has no latency, the FFT tails start a block later and the
    // biggest block is capped so we don't get long FFT spikes
    struct Tail {
        using Complex = std::complex<float>;
        std::size_t size{}, partitions{}, input_cursor{}, output_cursor{},
                slot{};
        std::vector<std::pair<std::uint32_t, std::uint32_t>> permutation_swaps;
        std::vector<Complex> roots;
        std::array<std::vector<Complex>, 2> spectra, history, work;
        std::array<std::vector<float>, 2> input, output;
        void transform(std::vector<Complex> &data, bool inverse) const;
        void reset();
        std::pair<float, float> process(float left, float right);
    };
    std::vector<Tail> tails;
    std::size_t response_frames{};
    [[nodiscard]] float convolve(
            std::size_t impulse_channel, std::size_t input_channel) const;

    std::vector<std::vector<float>> impulses;
    std::vector<float> left_history;
    std::vector<float> right_history;
    std::size_t cursor{};
};

} // namespace ekphore::irc

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace ekphore::irc {

#if defined(_MSC_VER)
#pragma float_control(precise, on, push)
#pragma fp_contract(off)
#endif

// resampling mode 7, NOT the effect's own rate converter

#if defined(__GNUC__) && !defined(__clang__)
__attribute__((optimize("fp-contract=off")))
#endif
inline std::vector<float> import_channel(
        std::span<const float> source, float source_rate, float target_rate) {
#if defined(__clang__)
#pragma clang fp contract(off)
#endif
    if (!(source_rate > 0) || !(target_rate > 0) ||
            !std::isfinite(source_rate) || !std::isfinite(target_rate))
        throw std::invalid_argument("invalid IR import rate");
    if (source_rate == target_rate)
        return { source.begin(), source.end() };
    const float ratio = target_rate / source_rate;
    const float step = 1.0F / ratio;
    // just so the math doesn't blow up
    if (!std::isfinite(step) || step > 1000.0F || ratio > 1000.0F)
        throw std::invalid_argument(
                "IR import ratio exceeds arithmetic bounds");
    constexpr int length = 1792;
    std::array<float, length> table{}, diff{};
    const auto series = [](float v) {
        float term = 1.0F, sum = 1.0F;
        int k = 1;
        do {
            const float q = v / static_cast<float>(k++);
            term *= q * q;
            sum += term;
        } while (static_cast<double>(sum) * 1e-21 <= term);
        return sum;
    };
    constexpr float cutoff = 0x1.cf9998p-1F;
    const float denominator = 1.0F / series(4.5F);
    table[0] = cutoff;
    for (int i = 1; i < length; ++i) {
        const float x = static_cast<float>(i) / 256.0F;
        const float p = x * 0x1.921fb6p+1F;
        const float sinc = std::sin(p * cutoff) / p;
        const float z = (1.0F / 1791.0F) * static_cast<float>(i);
        const float v = (std::sqrt(1.0F - z * z) * 9.0F) * 0.5F;
        table[i] = sinc * (denominator * series(v));
        diff[i - 1] = table[i] - table[i - 1];
    }
    const int table_step = static_cast<int>(
            static_cast<double>(std::min(ratio * 256.0F, 256.0F) * 128.0F) +
            0.5);
    if (table_step < 1)
        throw std::invalid_argument("IR import table step is zero");
    const float scale = ratio < 1.0F ? ratio * 0.99F : 0.99F;
    // offset so negative indices still work
    const int h = static_cast<int>(
            std::max(16.0F, std::max(step, 1.0F) * 8.0F + 1.0F));
    std::vector<float> storage(1024 + 8 * h, 0.0F);
    float *buffer = storage.data() + 2 * h;
    int append = h, history = h, pending = 0;
    double phase = h;
    bool first = true;
    std::vector<float> output;
    output.reserve(static_cast<std::size_t>(source.size() * ratio) + 1);
    const auto coefficient = [&](int t) {
        return table[t >> 7] +
               diff[t >> 7] * (static_cast<float>(t & 127) * 0.0078125F);
    };
    for (std::size_t offset = 0; offset < source.size(); offset += 1024) {
        const int count = static_cast<int>(
                std::min<std::size_t>(1024, source.size() - offset));
        std::copy_n(source.data() + offset, count, buffer + append + pending);
        const int available = pending + count;
        int usable = available;
        if (first) {
            usable -= h;
            first = false;
        }
        const int buffered = append + available;
        const int upsample_limit = static_cast<int>(
                std::nearbyint((static_cast<float>(usable) * ratio) * 100.0F));
        const auto admits = [&](int n) {
            // clang-format off
            return ratio < 1.0F ? static_cast<int>(std::nearbyint((step *
                    static_cast<float>(n)) * 100.0F)) < usable * 100 : n * 100 <
                    upsample_limit;
            // clang-format on
        };
        for (int n = 0; admits(n); ++n) {
            const auto q = static_cast<std::int64_t>(phase * 32768.0);
            const int center = static_cast<int>(q >> 15);
            const auto fraction = static_cast<std::uint32_t>(q) & 32767;
            int t = static_cast<int>((fraction * table_step) >> 15);
            float left = 0.0F, right = 0.0F;
            for (int j = center; t < length * 128; t += table_step, --j)
                left += buffer[j] * coefficient(t);
            const auto fraction_right = static_cast<std::uint32_t>(-q) & 32767;
            t = static_cast<int>((fraction_right * table_step) >> 15);
            if (fraction_right == 0)
                t += table_step;
            for (int j = center + 1; t < length * 128 - 128;
                    t += table_step, ++j)
                right += buffer[j] * coefficient(t);
            output.push_back((left + right) * scale);
            phase += static_cast<double>(step);
        }
        pending = buffered - (2 * h + usable);
        phase -= static_cast<double>(usable);
        history += usable;
        const int carry = static_cast<int>(phase - h);
        phase -= static_cast<double>(carry);
        history += carry;
        const int new_append = usable + 3 * h - history;
        for (int i = -h; i < new_append + pending; ++i)
            buffer[i] = buffer[i + history - h];
        append = new_append;
        history = h;
    }
    return output; // the tail never gets flushed
}

#if defined(_MSC_VER)
#pragma float_control(pop)
#endif

} // namespace ekphore::irc

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace ekphore::irc {

// the effect rebuild always uses pitch 1 for both
struct BranchParameters {
    float early_stretch{ 1.F }, late_stretch{ 1.F };
    float early_pitch{ 1.F }, late_pitch{ 1.F };
    float early_low_pass_hz{ 20000.F }, early_high_pass_hz{ 20.F };
    float late_low_pass_hz{ 20000.F }, late_high_pass_hz{ 20.F };
    float crosspoint{ -1.F };
    // zero means from the decimation controls, otherwise it has to match
    std::uint32_t processing_sample_rate{};
    std::optional<float> sample_rate_decimation;
    std::optional<bool> preserve_ir_length;
    friend bool operator==(
            const BranchParameters &, const BranchParameters &) = default;
};

// fill this under the engine lock, it must NOT point into Player
struct FilePreparationRequest {
    std::filesystem::path resolved_path;
    std::string requested_path;
    std::size_t convolution_block_size{};
    bool reverse{};
    bool auto_gain{};
    // zero keeps the file's rate
    std::uint32_t engine_sample_rate{};
    float pre_delay_ms{};
    Envelope envelope;
    std::optional<BranchParameters> branches;
    // must not point into Player
    std::function<std::optional<std::vector<std::byte>>(std::string_view)>
            resource_reader;
};

struct RatePlan {
    std::uint32_t import_rate{}, processing_rate{};
    unsigned divisor{ 1 };
};
// TODO: other ratios need their own converter, nullopt for now
[[nodiscard]] std::optional<RatePlan> rate_plan(
        const BranchParameters &parameters, std::uint32_t engine_rate,
        std::uint32_t decoded_rate) noexcept;

// allocates, NEVER from audio. check branch_preparation_supported to know which
// path you got
[[nodiscard]] bool branch_preparation_supported(
        const BranchParameters &parameters, std::uint32_t rate,
        std::size_t channels, std::uint32_t decoded_rate = 0) noexcept;
[[nodiscard]] std::vector<std::vector<float>> prepare_response(
        std::span<const std::vector<float>> channels,
        std::uint32_t decoded_rate, const FilePreparationRequest &request);

struct PreparedFile {
    PreparedFile() = default;
    PreparedFile(PreparedFile &&) noexcept = default;
    PreparedFile &operator=(PreparedFile &&) noexcept = default;
    PreparedFile(const PreparedFile &) = delete;
    PreparedFile &operator=(const PreparedFile &) = delete;
    PairedConvolver convolver;
    std::optional<std::string> requested_path;

    // stop processing first. the old convolver ends up in here, give it back
    // to the worker to free it
    void exchange(PairedConvolver &target,
            std::optional<std::string> &target_path) noexcept;
};

// blocks and allocates, throws on failure. success doesn't mean it got
// committed
[[nodiscard]] PreparedFile prepare_file(const FilePreparationRequest &request);

} // namespace ekphore::irc

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <vector>

namespace ekphore::irc {

struct AutomaticIrAnalysis {
    std::size_t crosspoint{};                // before reversing the response
    std::int32_t trim_start{}, trim_end{};   // rebuilding doesn't apply these
    std::int32_t decay_start{}, decay_end{}; // source frames
};

namespace analysis_detail {
// separate float ops even with contraction on
inline float add(float a, float b) {
    volatile float r = a + b;
    return r;
}
inline float sub(float a, float b) {
    volatile float r = a - b;
    return r;
}
inline float mul(float a, float b) {
    volatile float r = a * b;
    return r;
}
inline float div(float a, float b) {
    volatile float r = a / b;
    return r;
}
inline std::optional<std::int32_t> integer(float x) {
    if (!std::isfinite(x) || x < -0x1p31F || x >= 0x1p31F)
        return std::nullopt;
    return static_cast<std::int32_t>(x); // truncate, NOT round or floor
}
inline float word(std::uint32_t bits) { return std::bit_cast<float>(bits); }
} // namespace analysis_detail

// only the default case, zero thresholds and unity controls at the same rate.
// no resampling, reversing, trimming or stretching, and anything under 302 ms
// gets rejected

inline std::optional<AutomaticIrAnalysis> analyze_automatic_crosspoint(
        std::span<const std::vector<float>> channels, float sample_rate) {
    using namespace analysis_detail;
    if (channels.empty() || channels.size() > std::size_t(INT32_MAX) ||
            !std::isfinite(sample_rate) || sample_rate <= 0 ||
            std::trunc(sample_rate) != sample_rate)
        return std::nullopt;
    const auto size = channels.front().size();
    if (!size || size > std::size_t(INT32_MAX))
        return std::nullopt;
    for (const auto &lane : channels)
        if (lane.size() != size)
            return std::nullopt;
    const auto n = static_cast<std::int32_t>(size);
    const float bin = mul(sample_rate, word(0x3b03126f));    // 0.002f
    const float window = mul(sample_rate, word(0x3e99999a)); // 0.3f
    const auto hop_value = integer(bin), window_value = integer(window);
    const auto allocation = integer(div(sub(float(n), bin), bin));
    if (!hop_value || !window_value || !allocation || *hop_value < 1 ||
            *window_value < 2 || *allocation < 1)
        return std::nullopt;
    const std::int32_t hop = *hop_value, width = *window_value;
    std::vector<float> signal(size, 0.F), energy(std::size_t(*allocation), 0.F);
    double total = 0;
    for (std::size_t i = 0; i < size; ++i) {
        float value = 0;
        for (const auto &lane : channels) {
            if (!std::isfinite(lane[i]))
                return std::nullopt;
            value = add(lane[i], value); // channel order affects rounding
        }
        if (!std::isfinite(value))
            return std::nullopt;
        signal[i] = value;
        total += double(value); // double, narrow at the end
    }
    const float mean = float(total / double(n));
    const float inverse = div(1.F, float(channels.size()));
    for (float &value : signal) {
        value = mul(inverse, sub(value, mean));
        value = value > 0.F ? value : -value;
        if (!std::isfinite(value))
            return std::nullopt;
    }
    // first/last nonzero, one frame of margin at the start and the end is
    // exclusive
    std::int32_t start = 0, end = n;
    for (std::int32_t i = 0; i < n; ++i)
        if (signal[std::size_t(i)] != 0.F) {
            start = std::max(0, i - 1);
            break;
        }
    for (std::int32_t i = n - 1; i >= 1; --i)
        if (signal[std::size_t(i)] != 0.F) {
            end = i;
            break;
        }
    const auto length = end - start;
    // the short case is a different algorithm, don't just use the long one
    if (std::int64_t(length) < std::int64_t(width) + hop)
        return std::nullopt;
    const float length_f = float(length);
    const float scan_f = div(sub(length_f, window), bin);
    const auto scan_value = integer(scan_f);
    const auto last_value = integer(add(div(sub(length_f, bin), bin), -1.F));
    const auto scores_value = integer(add(scan_f, -1.F));
    if (!scan_value || !last_value || !scores_value || *scan_value < 1 ||
            *last_value < 0 || *scores_value < 1)
        return std::nullopt;
    const std::size_t scans = std::size_t(*scan_value);
    const std::size_t last = std::size_t(*last_value);
    const std::size_t scores = std::size_t(*scores_value);
    if (scans > energy.size() || last >= energy.size() ||
            std::int64_t(start) + (std::int64_t(last) + 1) * hop > n ||
            std::int64_t(start) + std::int64_t(scores - 1) * hop + width > n ||
            std::int64_t(start) + std::int64_t(scores) > n)
        return std::nullopt;
    for (float &value : signal) {
        value = mul(value, value);
        if (!std::isfinite(value))
            return std::nullopt;
    }
    // sum nonoverlapping trunc(bin)-frame buckets, then accumulate backwards;
    // a forward prefix sum would change the rounding
    for (std::size_t i = 0; i <= last; ++i) {
        float sum = 0;
        const auto first = std::size_t(start) + i * std::size_t(hop);
        for (std::int32_t j = 0; j < hop; ++j)
            sum = add(sum, signal[first + std::size_t(j)]);
        if (!std::isfinite(sum))
            return std::nullopt;
        energy[i] = sum;
    }
    for (std::size_t i = last; i > 0; --i) {
        energy[i - 1] = add(energy[i], energy[i - 1]);
        if (!std::isfinite(energy[i - 1]))
            return std::nullopt;
    }
    // energy[0] gets read again on EVERY divide and goes last
    if (energy[0] > 0.F)
        for (std::size_t i = scans; i > 0; --i)
            energy[i - 1] = div(energy[i - 1], energy[0]);
    // score sliding 300ms windows; square the already squared signal again,
    // keeping the sums in float
    for (std::size_t i = 0; i < scores; ++i) {
        const auto first = std::size_t(start) + i * std::size_t(hop);
        float sum = 0, squares = 0;
        for (std::int32_t j = 0; j < width; ++j) {
            const float value = signal[first + std::size_t(j)];
            sum = add(sum, value);
            squares = add(squares, mul(value, value));
        }
        float score = 0;
        if (sum != 0.F) {
            const float average = div(sum, float(width));
            score = div(squares, mul(mul(average, average), float(width - 1)));
        }
        if (!std::isfinite(sum) || !std::isfinite(squares) ||
                !std::isfinite(score))
            return std::nullopt;
        // in place, the later windows still need the squared samples after
        signal[std::size_t(start) + i] = score;
    }
    const auto crossing = [&](float threshold, std::size_t from = 0) {
        auto i = from;
        while (i < scans && energy[i] > threshold)
            ++i;
        return i;
    };
    const auto a = crossing(word(0x3a83126f));    // 0.001f, an index
    const auto b = crossing(word(0x358637bd), a); // 1e-6f
    // the scans can read past the scores, so keep that data around
    if (std::size_t(start) + b >= signal.size() ||
            std::size_t(start) + a > signal.size())
        return std::nullopt;
    float average = 0;
    for (std::size_t i = 0; i < a; ++i)
        average = add(average, signal[std::size_t(start) + i]);
    average = div(average, float(std::max<std::size_t>(a, 1)));
    if (!std::isfinite(average))
        return std::nullopt;
    std::size_t left = 0;
    while (left < b && signal[std::size_t(start) + left] > average)
        ++left;
    // start at b; don't read index 0 on the backwards scan
    auto right = b;
    while (right >= 1 && signal[std::size_t(start) + right] < average)
        --right;
    if (left + right > std::size_t(INT32_MAX))
        return std::nullopt;
    const float choice =
            right < a ? mul(float(left + right), .5F) : float(left);
    const auto first_choice = integer(add(mul(bin, choice), .5F));
    const auto second_choice = integer(add(mul(bin, float(right)), .5F));
    if (!first_choice || !second_choice)
        return std::nullopt;
    const auto fallback =
            right > a ? std::max(n / 2 - start, 0) : *second_choice;
    const std::int64_t c =
            std::int64_t(start) + (left <= a ? *first_choice : fallback);
    const auto rounded_bin = integer(add(bin, .5F));
    const auto margin = integer(add(mul(sample_rate, word(0x3ecccccd)), .5F));
    if (!rounded_bin || !margin || c < 0 || c > n)
        return std::nullopt;
    const auto d0 = crossing(word(0x3ea1e89b)); // -5dB energy threshold
    const auto d1 = crossing(word(0x39a5cb5f)); // -35dB energy threshold
    const std::int64_t decay0 = std::int64_t(d0) * *rounded_bin;
    const std::int64_t decay1 = std::int64_t(d1) * *rounded_bin;
    const std::int64_t trim0 = std::int64_t(start) - *rounded_bin;
    const std::int64_t trim1 = std::int64_t(end) + *margin;
    if (decay0 > INT32_MAX || decay1 > INT32_MAX || trim0 < INT32_MIN ||
            trim1 > INT32_MAX)
        return std::nullopt;
    return AutomaticIrAnalysis{ std::size_t(c), std::int32_t(trim0),
        std::int32_t(trim1), std::int32_t(decay0), std::int32_t(decay1) };
}

inline std::optional<std::size_t> automatic_crosspoint(
        std::span<const std::vector<float>> channels, float sample_rate) {
    const auto result = analyze_automatic_crosspoint(channels, sample_rate);
    if (!result)
        return std::nullopt;
    return result->crosspoint;
}

} // namespace ekphore::irc

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace ekphore::irc::diagnostic {
// diagnostic only, NOT for realtime. it stays attached until the convolver is
// gone even after the scope ends, and copies share the counters so keep them
// all on one thread
struct ComparisonRow {
    std::size_t ir_frames{}, channels{};
    unsigned divisor{ 1 };
    bool actual_staged{};
    std::uint64_t calls{}, different_samples{}, nonfinite_samples{};
    std::array<double, 2> square_error{}, actual_square{}, shadow_square{};
    std::array<double, 2> maximum_error{};
};
struct ComparisonCollector {
    std::vector<std::shared_ptr<ComparisonRow>> rows;
};
inline thread_local ComparisonCollector *active_comparison{};
class ComparisonCapture {
    ComparisonCollector *previous;

  public:
    explicit ComparisonCapture(ComparisonCollector &collector) noexcept
        : previous(active_comparison) {
        active_comparison = &collector;
    }
    ~ComparisonCapture() { active_comparison = previous; }
    ComparisonCapture(const ComparisonCapture &) = delete;
    ComparisonCapture &operator=(const ComparisonCapture &) = delete;
};
struct ComparisonState;
} // namespace ekphore::irc::diagnostic

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace ekphore::irc {

// dry only, NOT the wet input
class DryAlignmentDelay {
  public:
    // our own limit, anything bigger returns false and changes nothing
    static constexpr std::size_t max_delay_frames = 1U << 20;

    [[nodiscard]] bool configure(bool enabled, float engine_rate,
            float processing_rate, std::int32_t block) {
        std::size_t delay = 0;
        if (enabled) {
            if (!std::isfinite(engine_rate) ||
                    !std::isfinite(processing_rate) || engine_rate <= 0.F ||
                    processing_rate <= 0.F || block < 0 || block > 1024)
                return false;
            // float divide, multiply, truncate, and check BEFORE the cast
            volatile float ratio = engine_rate / processing_rate;
            const float frames = ratio * float(block);
            if (!std::isfinite(frames) || frames < 0.F ||
                    frames > float(max_delay_frames))
                return false;
            delay = static_cast<std::size_t>(frames);
            // TODO: this can truncate to zero and we don't know what happens
            // then, don't assume it passes through
            if (block > 0 && delay == 0)
                return false;
        }
        // fresh zeroed history
        std::vector<std::pair<float, float>> prepared;
        if (delay)
            prepared.resize(delay + 1);
        ring.swap(prepared);
        alignment_frames = delay;
        cursor = 0;
        return true;
    }

    [[nodiscard]] std::pair<float, float> process(
            float left, float right) noexcept {
        if (!alignment_frames)
            return { left, right };
        // write at cursor+D, then read and clear at cursor, D+1 is enough per
        // sample
        std::size_t target = cursor + alignment_frames;
        if (target >= ring.size())
            target -= ring.size();
        ring[target] = { left, right };
        const auto result = ring[cursor];
        ring[cursor] = {};
        if (++cursor == ring.size())
            cursor = 0;
        return result;
    }

    void reset() noexcept {
        std::fill(ring.begin(), ring.end(), std::pair<float, float>{});
    }

    [[nodiscard]] std::size_t delay_frames() const noexcept {
        return alignment_frames;
    }

  private:
    std::vector<std::pair<float, float>> ring;
    std::size_t alignment_frames{};
    std::size_t cursor{};
};

} // namespace ekphore::irc

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace ekphore::irc {

enum class IrFilterKind { low_pass, high_pass };
struct IrFilterCoefficients {
    float b0{}, b1{}, b2{}, a1{}, a2{};
    bool bypass{};
};

namespace ir_filter_detail {
inline float add(float a, float b) {
    volatile float r = a + b;
    return r;
}
inline float sub(float a, float b) {
    volatile float r = a - b;
    return r;
}
inline float mul(float a, float b) {
    volatile float r = a * b;
    return r;
}
inline float div(float a, float b) {
    volatile float r = a / b;
    return r;
}
} // namespace ir_filter_detail

// modes 3/4, only for preparing the IR
inline IrFilterCoefficients ir_filter_coefficients(
        IrFilterKind kind, float frequency, float sample_rate) {
    using namespace ir_filter_detail;
    if (!std::isfinite(frequency) || frequency < 0.F ||
            !std::isfinite(sample_rate) || sample_rate < 1.F ||
            sample_rate >= 0x1p31F)
        throw std::invalid_argument("invalid IRC filter frequency/rate");
    // the rate goes through an int first, then back to float
    const float native_rate =
            static_cast<float>(static_cast<std::int32_t>(sample_rate));
    const float u = div(frequency, native_rate);
    const float k = std::tan(mul(u, std::bit_cast<float>(0x40490fdbU)));
    const float t = mul(k, k);
    const float damping = mul(k, std::bit_cast<float>(0x3fb504f3U));
    // t+(damping+1), NOT (t+damping)+1
    const float d = div(1.F, add(t, add(damping, 1.F)));
    const float twice_d = add(d, d);
    IrFilterCoefficients c;
    if (kind == IrFilterKind::low_pass) {
        c.b0 = c.b2 = mul(t, d);
        c.b1 = mul(t, twice_d);
        c.bypass = u > std::bit_cast<float>(0x3ee66666U);
    } else {
        c.b0 = c.b2 = d;
        c.b1 = mul(d, -2.F);
        c.bypass = u < std::bit_cast<float>(0x3c23d70aU);
    }
    c.a1 = -mul(add(t, -1.F), twice_d);
    c.a2 = -mul(add(t, sub(1.F, damping)), d);
    return c;
}

// zero history per channel, checks everything BEFORE touching anything
inline void apply_ir_filter(std::span<std::vector<float>> channels,
        std::size_t frames, const IrFilterCoefficients &c) {
    using namespace ir_filter_detail;
    for (const auto &plane : channels)
        if (plane.size() < frames)
            throw std::invalid_argument("IRC filter plane shorter than extent");
    if (c.bypass)
        return;
    constexpr float tiny = std::bit_cast<float>(0x179abe15U);
    for (auto &plane : channels) {
        float x1 = 0.F, x2 = 0.F, y1 = 0.F, y2 = 0.F;
        for (std::size_t i = 0; i < frames; ++i) {
            const float x = plane[i];
            const float feed = add(
                    add(add(mul(c.b0, x), mul(c.b1, x1)), mul(c.b2, x2)), tiny);
            // the two feedback products get added together first
            const float y = add(feed, add(mul(c.a1, y1), mul(c.a2, y2)));
            x2 = x1;
            x1 = x;
            y2 = y1;
            y1 = y;
            plane[i] = y;
        }
    }
}
} // namespace ekphore::irc

#include <cstddef>
#include <span>
#include <vector>

namespace ekphore::irc {
struct MergeParameters {
    float sample_rate{}, processing_rate{};
    float early_stretch{ 1.F }, late_stretch{ 1.F }, crossfade_seconds{ .05F };
    std::size_t adapted_frames{}, crosspoint{};
};
// allocates. the stored stretch, NOT the quantized PV one
[[nodiscard]] std::vector<std::vector<float>> merge_early_late(
        std::span<const std::vector<float>> early,
        std::span<const std::vector<float>> late,
        const MergeParameters &parameters);
} // namespace ekphore::irc
