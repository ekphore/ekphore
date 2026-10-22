// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <memory>
#include <span>
#include <vector>

namespace ekphore::irc::pv {
struct FreshCoefficients {
    std::vector<float> window;        // K values
    std::vector<float> phase_step;    // N values
    std::vector<float> normalization; // K values
};
// doesn't touch the histories. K even and >=2, N a power of two >=4,
// 1<=analysisHop<=K, normalizationHop>0, throws otherwise
[[nodiscard]] FreshCoefficients prepare_fresh_coefficients(std::size_t K,
        std::size_t N, std::size_t analysisHop, std::size_t normalizationHop);

// separate float ops, floor and NOT fmod
[[nodiscard]] float principal_argument(float value) noexcept;
void principal_argument(std::span<float> values) noexcept;

#if defined(__APPLE__)
// real half then imaginary half, DC and Nyquist at real[0]/imaginary[0]. NOT
// from audio, not thread safe, and only Apple's FFT for now
class RealFft {
  public:
    explicit RealFft(std::size_t size);
    ~RealFft();
    RealFft(const RealFft &) = delete;
    RealFft &operator=(const RealFft &) = delete;
    void forward(std::span<float> data);
    void inverse(std::span<float> data);
    [[nodiscard]] std::size_t size() const noexcept { return fft_size; }

  private:
    struct PlaneDeleter {
        void operator()(float *) const noexcept;
    };
    using Plane = std::unique_ptr<float[], PlaneDeleter>;
    std::size_t fft_size, log2_size;
    void *setup{};
    Plane real, imaginary, scratch;
};
#endif
} // namespace ekphore::irc::pv

#include <cstddef>
#include <span>
#include <vector>

namespace ekphore::irc::pv {
// the leftover entries and sentinel reads matter, don't clean them up
class Regions {
  public:
    explicit Regions(std::size_t spectral_size);
    std::size_t select(std::span<const float> magnitude,
            std::span<const float> frequency, unsigned analysis_hop);
    std::size_t select(std::span<const float> magnitude,
            std::span<const float> frequency, unsigned analysis_hop,
            std::size_t usable_bins);
    void reset_tables() noexcept; // clears the entries, keeps the count
    [[nodiscard]] std::span<const int> map() const noexcept {
        return { bin_map.data(), bins };
    }
    [[nodiscard]] std::span<const int> peaks() const noexcept {
        return { candidate.data(), count };
    }

  private:
    std::size_t spectral_size, bins, count{};
    std::vector<int> bin_map, candidate, scratch;
};
} // namespace ekphore::irc::pv

#include <cstdint>
#include <span>
#include <vector>

namespace ekphore::irc::pv {
#if defined(__APPLE__)
// K=N, needs strict FP. buffering and resampling are in ElastiqueStream
class FrameProcessor {
  public:
    FrameProcessor(std::size_t size, std::size_t analysis_hop,
            std::size_t output_hop, std::size_t normalization_hop,
            std::size_t channels, float stretch);
    void reset(); // counts and channel 0 phase stay
    void set_cutoff(std::size_t usable_bins);
    void set_stretch(float stretch); // doesn't update the hops
    // history stays
    void set_hops(unsigned output_hop, unsigned analysis_hop,
            unsigned normalization_hop, float stretch);
    // only shifts this channel in, no FFT and nothing advances
    unsigned prime_channel(std::size_t channel, std::span<const float> input);
    void prime(std::span<const std::span<const float>> input); // stage -1
    // you pick the order. stage 0 reads analysisHop, stage 4 writes outputHop,
    // the rest don't touch input/output
    void run_stage(std::size_t channel, unsigned stage,
            std::span<const float> input = {}, std::span<float> output = {});
    // five stages per channel, channel 0 first. nothing can alias
    void process(std::span<const std::span<const float>> input,
            std::span<const std::span<float>> output);
    [[nodiscard]] static bool cutoff_supported() noexcept;
    [[nodiscard]] std::uint32_t counter() const noexcept {
        return processed_count;
    }
    [[nodiscard]] std::size_t selected_count() const noexcept {
        return num_selected;
    }
    [[nodiscard]] float previous_stretch() const noexcept {
        return last_stretch;
    }
    [[nodiscard]] float current_stretch() const noexcept {
        return stretch_ratio;
    }
    [[nodiscard]] unsigned output_hop() const noexcept {
        return unsigned(synthesis_hop);
    }
    [[nodiscard]] unsigned analysis_hop() const noexcept {
        return unsigned(analysis_stride);
    }
    [[nodiscard]] std::size_t channels() const noexcept {
        return channel_data.size();
    }
    [[nodiscard]] std::span<const float> last_phase(std::size_t ch) const;
    [[nodiscard]] std::span<const float> overlap(std::size_t ch) const;
    [[nodiscard]] std::span<const float> analysis_spectrum(
            std::size_t ch) const;
    [[nodiscard]] std::span<const float> reconstructed_spectrum(
            std::size_t ch) const;
    [[nodiscard]] std::span<const float> current_phase() const {
        return phase_values;
    }
    [[nodiscard]] std::span<const float> frequency() const {
        return frequency_values;
    }
    [[nodiscard]] std::span<const float> offsets() const { return offset; }
    [[nodiscard]] std::span<const float> synthesis() const {
        return synthesis_values;
    }
    [[nodiscard]] std::span<const float> transient_flags() const {
        return flags;
    }
    [[nodiscard]] std::span<const int> peaks() const {
        return { peak_indices.data(), num_selected };
    }
    [[nodiscard]] std::span<const int> map() const { return regions.map(); }
    [[nodiscard]] std::span<const float> retained_channel0_phase() const {
        return previous_phase;
    }
    [[nodiscard]] std::span<const float> retained_channel0_synthesis() const {
        return retained_synthesis;
    }

  private:
    struct Channel {
        std::vector<float> input, spectrum, reconstructed, overlap, phase,
                last_phase;
    };
    void validate_input(std::span<const std::span<const float>>) const;
    void append(Channel &, std::span<const float>);
    void analyze(Channel &);
    void phase_and_magnitude(Channel &, std::size_t);
    void synthesize(Channel &, std::size_t, float);
    void finish(Channel &, std::span<float>);
    std::size_t spectral_size, half_size, analysis_stride, synthesis_hop,
            normalization_stride, bin_limit;
    float stretch_ratio, last_stretch;
    std::uint32_t processed_count{};
    std::size_t num_selected{};
    FreshCoefficients coefficients;
    RealFft fft;
    Regions regions;
    std::vector<Channel> channel_data;
    std::vector<float> work, magnitude, quarter_history, regular_history,
            phase_values, synthesis_values, previous_phase, retained_synthesis,
            frequency_values, offset, cosine, sine, flags, ratios;
    std::vector<int> transient_work, peak_indices;
};

#endif
} // namespace ekphore::irc::pv

#include <cstddef>
#include <span>

namespace ekphore::irc::pv {
// views only, these don't own or advance the histories. N/2 bins, peaks can be
// empty
struct TransientInput {
    std::span<const int> map, peaks;
    std::span<const float> magnitude;
    std::span<const float> quarter_hop_history;
    std::span<const float> regular_history;
};
struct TransientOutput {
    std::span<float> flags, ratio;
    std::span<int> work;
};
// channel 0 BEFORE its peak phases. previousStretch picks the history and gets
// replaced even without peaks. nothing can alias, throws before writing
void process_transients(std::size_t K, std::size_t N, std::size_t analysisHop,
        float currentStretch, float &previousStretch, const TransientInput &,
        const TransientOutput &);

struct SyncInput {
    std::span<const int> peaks;
    std::span<const float> channel0_retained_synthesis;
    std::span<const float> current_phase;
    std::span<const float> channel0_previous_phase;
};
struct SyncOutput {
    std::span<float> synthesis_phase, phase_offset, cosine, sine;
};
// not channel 0, and channel 0's stage 3 has to have run already
void synchronize_channels(std::size_t N, const SyncInput &, const SyncOutput &);
} // namespace ekphore::irc::pv

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ekphore::irc::pv {
// type 1 only, needs -ffp-contract=off and no fast-math
class FastResample {
  public:
    FastResample(std::size_t channels, std::size_t input_capacity);
    void reset() noexcept;
    // doesn't change anything, throws if it doesn't fit
    [[nodiscard]] std::size_t output_count(
            std::size_t input_count, float ratio, std::size_t limit) const;
    // input can alias output since we copy it first, outputs can't alias each
    // other
    std::size_t process(std::span<const std::span<const float>> input,
            std::size_t input_count, std::span<const std::span<float>> output,
            float ratio);
    [[nodiscard]] double phase() const noexcept { return resample_phase; }
    [[nodiscard]] float fraction() const noexcept { return resample_fraction; }
    [[nodiscard]] std::int32_t cursor() const noexcept { return read_cursor; }
    [[nodiscard]] std::array<float, 3> history(std::size_t channel) const;

  private:
    std::size_t buffer_capacity;
    // four floats in front, only [-3..-1] get used. reset clears [0] too
    std::vector<std::vector<float>> planes;
    double resample_phase{};
    float resample_fraction{};
    std::int32_t read_cursor{};
};
} // namespace ekphore::irc::pv

#include <array>
#include <memory>
#include <span>
#include <vector>

namespace ekphore::irc::pv {
// negative stages are warmup, the rest go channel by channel
// (0..channels*5-1)
class StreamFrameBackend {
  public:
    virtual ~StreamFrameBackend() = default;
    virtual void initialize(std::size_t K, std::size_t channels,
            unsigned outputHop, unsigned analysisHop, unsigned normalizationHop,
            float initialStretch) = 0;
    virtual void set_hops(unsigned outputHop, unsigned analysisHop,
            unsigned normalizationHop, float stretch) = 0;
    virtual void set_cutoff(unsigned bin) = 0;
    virtual void reset() = 0;
    virtual unsigned process_stage(
            std::span<const std::span<const float>> input,
            std::span<const std::span<float>> output, int stage) = 0;
};
#if defined(__APPLE__)
class FrameProcessor;
// built once, new hops and warmup keep the history
class FrameStreamBackend final : public StreamFrameBackend {
  public:
    FrameStreamBackend();
    ~FrameStreamBackend() override;
    FrameStreamBackend(const FrameStreamBackend &) = delete;
    FrameStreamBackend &operator=(const FrameStreamBackend &) = delete;
    void initialize(std::size_t K, std::size_t channels, unsigned outputHop,
            unsigned analysisHop, unsigned normalizationHop,
            float initialStretch) override;
    void set_hops(unsigned outputHop, unsigned analysisHop,
            unsigned normalizationHop, float stretch) override;
    void set_cutoff(unsigned bin) override;
    void reset() override;
    unsigned process_stage(std::span<const std::span<const float>> input,
            std::span<const std::span<float>> output, int stage) override;
    [[nodiscard]] const FrameProcessor &frame() const;

  private:
    FrameProcessor &require_frame();
    std::unique_ptr<FrameProcessor> frame_processor;
};
#endif
struct StreamState {
    int buffered{}, output_position{}, drain_generated{}, initial_position{},
            alignment{};
    unsigned analysis_hop{}, output_hop{}, frame_output{}, cutoff_cap{};
    bool startup{}, forced_resample{};
    float combined_stretch{ 1 }, requested_pitch{ 1 }, current_pitch{ 1 },
            drift_bound{};
    double reciprocal{ 1 }, previous_reciprocal{ 1 }, input_position{};
    double nominal_total{}, actual_total{}, nominal_position{},
            resample_error{};
};
enum class StreamMode { standard = 0, pro = 2 };
// modes 0/2, mono or weighted stereo only. the backend has to outlive this
// and can't be shared
class ElastiqueStream {
  public:
    ElastiqueStream(float sample_rate, unsigned channels,
            unsigned output_quantum, StreamFrameBackend &backend,
            StreamMode mode = StreamMode::standard);
    void reset(); // keeps the buffers and the stretch
    // writes the quantized stretch back into yours
    void set_parameters(
            float &stretch, float pitch, bool preserve_position = false);
    void set_cutoff_frequency(float frequency);
    void set_output_offset(int position);
    void set_time_position(double position);
    // ProDirect clears this, it resamples on its own
    void set_force_resampling(bool enabled) noexcept {
        stream_state.forced_resample = enabled;
    }
    [[nodiscard]] unsigned frames_needed(); // changes the pitch estimate
    [[nodiscard]] unsigned frames_needed(unsigned output_quantum);
    [[nodiscard]] std::int64_t time_position() const;
    // null means drain, zero frames is NOT a drain. returns 0/-1, not a count
    int process(const std::span<const std::span<const float>> *input,
            unsigned count, std::span<const std::span<float>> output);
    // you pick the capacity, no latency compensation
    [[nodiscard]] std::vector<std::vector<float>> apply_offline(
            std::span<const std::span<const float>> source, float stretch,
            float pitch, std::size_t destination_capacity);
    [[nodiscard]] const StreamState &state() const noexcept {
        return stream_state;
    }
    [[nodiscard]] unsigned window_size() const noexcept { return K; }

  private:
    float configured_sample_rate;
    unsigned channel_count, output_quantum, K, buffer_capacity;
    StreamMode stream_mode;
    StreamFrameBackend &frame_backend;
    StreamState stream_state;
    FastResample resampler;
    std::vector<std::vector<float>> input_buffer, output_buffer;
    std::array<std::span<const float>, 2> input_views;
    std::array<std::span<float>, 2> output_views;
    void update_pitch(bool query);
    void advance_drift();
    void prepare_views();
    unsigned hop(bool drain);
    void consume_input();
    void flush(std::span<const std::span<float>> output);
};
} // namespace ekphore::irc::pv
