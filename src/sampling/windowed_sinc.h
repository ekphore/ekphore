// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <span>

namespace ekphore::sampling {

enum class SincQuality : std::uint8_t { high, ultra };

// the reader handles edges and loops, so this works for everything
class WindowedSinc final {
  public:
    using Reader = float (*)(const void *context, std::int64_t frame);

    [[nodiscard]] static float sample(double cursor,
            double source_frames_per_output_frame, SincQuality quality,
            const void *context, Reader reader);

    // read only
    [[nodiscard]] static std::span<const float> coefficients(
            SincQuality quality, bool downsampling);

    struct TableView {
        std::span<const float> values;
        std::int32_t last_phase{};
    };

  private:
    [[nodiscard]] static TableView table(
            SincQuality quality, bool downsampling);
};

} // namespace ekphore::sampling
