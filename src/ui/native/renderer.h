// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <ekphore/ui.h>

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace ekphore {

// TODO: fallback glyphs until we figure out Komplete UI's font provider
[[nodiscard]] std::array<std::uint8_t, 7> ui_fallback_glyph_rows(
        unsigned char character);

struct UiRasterizedText {
    int width{};
    int height{};
    std::vector<std::uint8_t> alpha;
};

// TODO: FreeType until we figure out the theme's font resolver, glyph grid if
// that fails too
[[nodiscard]] std::shared_ptr<const UiRasterizedText> ui_rasterize_builtin_text(
        std::string_view text, std::int32_t font_type,
        float pixel_density = 1.0F);
[[nodiscard]] std::shared_ptr<const UiRasterizedText> ui_rasterize_font_text(
        std::string_view text, std::span<const std::byte> font,
        std::uint32_t pixel_size);

// the frame has to be initialized already, KSP controls get painted elsewhere
void render_native_ui_components(const UiDocument &document,
        const UiAssetProvider *assets, UiFrame &frame, float scale,
        std::vector<NativeUiHitRegion> *hits = nullptr);

} // namespace ekphore
