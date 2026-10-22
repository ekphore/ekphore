// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

namespace ekphore::parametric_eq {

class Filter {
  public:
    bool configure(std::uint32_t sample_rate, std::size_t band_count,
            const std::array<std::optional<float>, 3> &frequency,
            const std::array<std::optional<float>, 3> &bandwidth,
            const std::array<std::optional<float>, 3> &gain);
    void reset();
    // once after reset, the modulation defaults stay
    void prepare();
    // normalized, NOT Hz/dB
    static std::array<float, 3> normalize(
            float frequency, float bandwidth, float gain);
    void set_normalized_band(std::size_t band,
            const std::array<float, 3> &values, bool modulated = false,
            unsigned component_mask = 7);
    // only spans longer than 31 samples use unfused vector arithmetic
    void begin_span(std::size_t frames) { use_vector_arithmetic = frames > 31; }
    static float process_band(float input,
            const std::array<float, 5> &coefficients,
            std::array<float, 4> &state, bool vector_span);

    [[nodiscard]] float process_channel(float input, std::size_t channel);
    [[nodiscard]] std::pair<float, float> process(float left, float right);
    [[nodiscard]] std::size_t band_count() const { return num_bands; }
    [[nodiscard]] bool active(std::size_t band) const;
    // all four histories, even flat bands
    [[nodiscard]] bool has_signal() const;
    [[nodiscard]] const std::array<float, 5> &coefficients(
            std::size_t band) const;

  private:
    std::size_t num_bands{};
    std::uint32_t configured_sample_rate{};
    bool use_vector_arithmetic{};
    bool dirty{};
    std::array<std::array<float, 3>, 3> band_values{};
    std::array<std::array<float, 5>, 3> band_coefficients{};
    std::array<bool, 3> active_bands{};
    std::array<std::array<std::array<float, 4>, 3>, 2> channel_states{};
};

} // namespace ekphore::parametric_eq
