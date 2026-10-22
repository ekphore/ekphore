// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <ekphore/ui.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <numeric>
#include <tuple>
#include <unordered_map>

#include <ft2build.h>
#include FT_FREETYPE_H

#include "embedded_fonts.h"
#include "native/renderer.h"

namespace ekphore {

void materialize_ui_native(UiDocument &document) {
    if (!document.captured_native_ui)
        return;
    const auto &evaluation = *document.captured_native_ui;
    document.native_ui_diagnostics.insert(document.native_ui_diagnostics.end(),
            evaluation.diagnostics.begin(), evaluation.diagnostics.end());
    document.native_ui_root_value_type = evaluation.root_value_type;
    document.native_ui_materialized_value_type =
            evaluation.materialized_value_type;
    document.native_ui_materialized_component_count =
            evaluation.materialized_component_count;
    document.native_ui_materialized_component_kinds =
            evaluation.materialized_component_kinds;
    document.native_ui_component_roots = evaluation.component_roots;
    if (document.background_picture.empty() &&
            !document.native_ui_component_roots.empty()) {
        const auto find_background = [&](const auto &self,
                                             const NativeUiComponentNode &node)
                -> std::optional<std::string> {
            if (node.kind == "native_ui.Image") {
                const auto file = std::ranges::find(node.properties,
                        std::string("file"), &NativeUiProperty::name);
                const bool explicitly_framed = std::ranges::any_of(
                        node.modifiers, [](const NativeUiModifier &modifier) {
                            return modifier.name == "frame";
                        });
                if (file != node.properties.end() && !explicitly_framed)
                    if (const auto value =
                                    std::get_if<std::string>(&file->value))
                        return *value;
            }
            for (const auto &child : node.children)
                if (auto result = self(self, child))
                    return result;
            return std::nullopt;
        };
        for (const auto &root : document.native_ui_component_roots)
            if (auto background = find_background(find_background, root)) {
                document.background_picture = std::move(*background);
                break;
            }
    }
    document.captured_native_ui.reset();
}

void materialize_ui_waveforms(UiDocument &document) {
    for (auto &control : document.controls) {
        if (!control.waveform_source)
            continue;
        const auto &channel = *control.waveform_source;
        const auto first_frame =
                std::min(control.waveform_first_frame, channel.size());
        const auto last_frame = std::clamp(
                control.waveform_last_frame, first_frame, channel.size());
        const auto displayed_frames = last_frame - first_frame;
        if (displayed_frames != 0) {
            const auto columns =
                    static_cast<std::size_t>(std::max(control.bounds.width, 1));
            control.waveform.resize(columns);
            for (std::size_t column = 0; column < columns; ++column) {
                const auto first =
                        first_frame + column * displayed_frames / columns;
                // clang-format off
                const auto last = std::min(first_frame + std::max((column + 1) *
                        displayed_frames / columns, column * displayed_frames /
                        columns + 1), last_frame);
                // clang-format on
                float minimum = 1.0F;
                float maximum = -1.0F;
                for (std::size_t frame = first; frame < last; ++frame) {
                    minimum = std::min(minimum, channel[frame]);
                    maximum = std::max(maximum, channel[frame]);
                }
                control.waveform[column] = { minimum, maximum };
            }
        }
        control.waveform_source.reset(); // free the samples here, not on audio
    }
}
std::array<std::uint8_t, 7> ui_fallback_glyph_rows(unsigned char character) {
    if (character >= 'a' && character <= 'z')
        character = static_cast<unsigned char>(character - 'a' + 'A');
    switch (character) {
        case 'A':
            return { 14, 17, 17, 31, 17, 17, 17 };
        case 'B':
            return { 30, 17, 17, 30, 17, 17, 30 };
        case 'C':
            return { 14, 17, 16, 16, 16, 17, 14 };
        case 'D':
            return { 30, 17, 17, 17, 17, 17, 30 };
        case 'E':
            return { 31, 16, 16, 30, 16, 16, 31 };
        case 'F':
            return { 31, 16, 16, 30, 16, 16, 16 };
        case 'G':
            return { 14, 17, 16, 23, 17, 17, 15 };
        case 'H':
            return { 17, 17, 17, 31, 17, 17, 17 };
        case 'I':
            return { 14, 4, 4, 4, 4, 4, 14 };
        case 'J':
            return { 7, 2, 2, 2, 18, 18, 12 };
        case 'K':
            return { 17, 18, 20, 24, 20, 18, 17 };
        case 'L':
            return { 16, 16, 16, 16, 16, 16, 31 };
        case 'M':
            return { 17, 27, 21, 21, 17, 17, 17 };
        case 'N':
            return { 17, 25, 21, 19, 17, 17, 17 };
        case 'O':
            return { 14, 17, 17, 17, 17, 17, 14 };
        case 'P':
            return { 30, 17, 17, 30, 16, 16, 16 };
        case 'Q':
            return { 14, 17, 17, 17, 21, 18, 13 };
        case 'R':
            return { 30, 17, 17, 30, 20, 18, 17 };
        case 'S':
            return { 15, 16, 16, 14, 1, 1, 30 };
        case 'T':
            return { 31, 4, 4, 4, 4, 4, 4 };
        case 'U':
            return { 17, 17, 17, 17, 17, 17, 14 };
        case 'V':
            return { 17, 17, 17, 17, 17, 10, 4 };
        case 'W':
            return { 17, 17, 17, 21, 21, 21, 10 };
        case 'X':
            return { 17, 17, 10, 4, 10, 17, 17 };
        case 'Y':
            return { 17, 17, 10, 4, 4, 4, 4 };
        case 'Z':
            return { 31, 1, 2, 4, 8, 16, 31 };
        case '0':
            return { 14, 17, 19, 21, 25, 17, 14 };
        case '1':
            return { 4, 12, 4, 4, 4, 4, 14 };
        case '2':
            return { 14, 17, 1, 2, 4, 8, 31 };
        case '3':
            return { 30, 1, 1, 14, 1, 1, 30 };
        case '4':
            return { 2, 6, 10, 18, 31, 2, 2 };
        case '5':
            return { 31, 16, 16, 30, 1, 1, 30 };
        case '6':
            return { 14, 16, 16, 30, 17, 17, 14 };
        case '7':
            return { 31, 1, 2, 4, 8, 8, 8 };
        case '8':
            return { 14, 17, 17, 14, 17, 17, 14 };
        case '9':
            return { 14, 17, 17, 15, 1, 1, 14 };
        case '-':
            return { 0, 0, 0, 31, 0, 0, 0 };
        case '_':
            return { 0, 0, 0, 0, 0, 0, 31 };
        case '.':
            return { 0, 0, 0, 0, 0, 6, 6 };
        case ':':
            return { 0, 6, 6, 0, 6, 6, 0 };
        case '/':
            return { 1, 2, 2, 4, 8, 8, 16 };
        case '+':
            return { 0, 4, 4, 31, 4, 4, 0 };
        case '#':
            return { 10, 10, 31, 10, 31, 10, 10 };
        case '%':
            return { 17, 2, 4, 8, 16, 17, 0 };
        case '(':
            return { 2, 4, 8, 8, 8, 4, 2 };
        case ')':
            return { 8, 4, 2, 2, 2, 4, 8 };
        case ' ':
            return {};
        default:
            return { 14, 17, 2, 4, 4, 0, 4 };
    }
}

namespace {

std::int64_t integer_value(const libksp::Value &value) {
    if (const auto *integer = std::get_if<std::int64_t>(&value))
        return *integer;
    if (const auto *real = std::get_if<double>(&value))
        return static_cast<std::int64_t>(std::llround(*real));
    return 0;
}

struct BuiltinFontSpec {
    std::uint8_t resource{};
    std::uint8_t pixel_size{};
};

std::optional<BuiltinFontSpec> builtin_font_spec(const std::int32_t type) {
    // Lumen BodyB, NOT a KSP FONT_TYPE
    if (type == -100)
        return BuiltinFontSpec{ 5, 12 };
    // one-based KSP types, NGL 38 is the only one out of order
    static constexpr std::array<BuiltinFontSpec, 25> specs{ {
            { 4, 11 },
            { 3, 12 },
            { 3, 12 },
            { 3, 12 },
            { 4, 11 },
            { 3, 12 },
            { 4, 11 },
            { 3, 12 },
            { 3, 12 },
            { 3, 12 },
            { 3, 12 },
            { 4, 10 },
            { 4, 10 },
            { 4, 10 },
            { 4, 10 },
            { 2, 12 },
            { 2, 12 },
            { 4, 10 },
            { 4, 11 },
            { 4, 11 },
            { 2, 12 },
            { 2, 11 },
            { 2, 11 },
            { 2, 11 },
            { 4, 10 },
    } };
    if (type < 1 || type > static_cast<std::int32_t>(specs.size()))
        return std::nullopt;
    return specs[static_cast<std::size_t>(type - 1)];
}

std::vector<FT_ULong> decode_utf8(
        const std::string_view text, const bool picture_font_bytes = false) {
    std::vector<FT_ULong> result;
    result.reserve(text.size());
    for (std::size_t index = 0; index < text.size();) {
        const auto lead = static_cast<unsigned char>(text[index++]);
        FT_ULong value{};
        std::size_t continuation{};
        if (lead < 0x80)
            value = lead;
        else if ((lead & 0xe0) == 0xc0) {
            value = lead & 0x1f;
            continuation = 1;
        } else if ((lead & 0xf0) == 0xe0) {
            value = lead & 0x0f;
            continuation = 2;
        } else if ((lead & 0xf8) == 0xf0) {
            value = lead & 0x07;
            continuation = 3;
        } else {
            result.push_back(picture_font_bytes ? lead : 0xfffd);
            continue;
        }
        const auto next = index;
        bool valid = index + continuation <= text.size();
        for (std::size_t count = 0; count < continuation; ++count) {
            if (!valid)
                break;
            const auto byte = static_cast<unsigned char>(text[next + count]);
            if ((byte & 0xc0) != 0x80) {
                valid = false;
                break;
            }
            value = (value << 6) | (byte & 0x3f);
        }
        valid = valid &&
                (continuation == 0 ||
                        (value >= (continuation == 1          ? 0x80U
                                          : continuation == 2 ? 0x800U
                                                              : 0x10000U) &&
                                value <= 0x10ffffU &&
                                !(value >= 0xd800U && value <= 0xdfffU)));
        // saved strings can have raw bytes for the 0..255 atlas, keep them if
        // they aren't UTF-8 and don't eat the next character
        if (valid)
            index += continuation;
        result.push_back(valid ? value : picture_font_bytes ? lead : 0xfffd);
    }
    return result;
}

std::optional<UiRasterizedText> rasterize_builtin_text(
        const std::string_view text, const std::int32_t font_type,
        const float pixel_density = 1.0F) {
    const auto spec = builtin_font_spec(font_type);
    if (!spec)
        return std::nullopt;
    const auto pixel_size = static_cast<std::uint32_t>(
            std::max(std::lround(spec->pixel_size *
                                 std::clamp(pixel_density, 0.25F, 4.0F)),
                    1L));
    const unsigned char *bytes{};
    std::size_t byte_count{};
    if (spec->resource == 5) {
#ifdef EKPHORE_HAS_PRIVATE_LUMEN_FONT
        bytes = embedded_fonts::lumen_medium;
        byte_count = sizeof(embedded_fonts::lumen_medium);
#else
        bytes = embedded_fonts::open_sans_semibold;
        byte_count = sizeof(embedded_fonts::open_sans_semibold);
#endif
    } else {
#ifdef EKPHORE_HAS_PRIVATE_KONTAKT_FONTS
        // 1 StandardJ, 2 RaldoSemiboldJ, 3 Standard0755J, 4 Pragmatica MediumTT
        switch (spec->resource) {
            case 1:
                bytes = embedded_fonts::resource_1;
                byte_count = sizeof(embedded_fonts::resource_1);
                break;
            case 2:
                bytes = embedded_fonts::resource_2;
                byte_count = sizeof(embedded_fonts::resource_2);
                break;
            case 3:
                bytes = embedded_fonts::resource_3;
                byte_count = sizeof(embedded_fonts::resource_3);
                break;
            case 4:
                bytes = embedded_fonts::resource_4;
                byte_count = sizeof(embedded_fonts::resource_4);
                break;
            default:
                return std::nullopt;
        }
#else
        // we can't ship the real fonts, Open Sans is close enough
        bytes = embedded_fonts::open_sans_semibold;
        byte_count = sizeof(embedded_fonts::open_sans_semibold);
#endif
    }

    struct FontCache {
        FT_Library library{};
        std::unordered_map<unsigned, FT_Face> faces;
        std::unordered_map<std::string, UiRasterizedText> text;
        std::mutex mutex;

        ~FontCache() {
            for (const auto &[key, face] : faces) {
                static_cast<void>(key);
                FT_Done_Face(face);
            }
            if (library)
                FT_Done_FreeType(library);
        }
    };
    static FontCache cache;
    std::lock_guard lock(cache.mutex);
    if (!cache.library && FT_Init_FreeType(&cache.library) != 0)
        return std::nullopt;
    const auto cache_key =
            static_cast<unsigned>(spec->resource) << 16 | pixel_size;
    auto found = cache.faces.find(cache_key);
    if (found == cache.faces.end()) {
        FT_Face created{};
        if (FT_New_Memory_Face(cache.library, bytes,
                    static_cast<FT_Long>(byte_count), 0, &created) != 0)
            return std::nullopt;
        if (FT_Select_Charmap(created, FT_ENCODING_UNICODE) != 0 ||
                FT_Set_Pixel_Sizes(created, pixel_size, 0) != 0) {
            FT_Done_Face(created);
            return std::nullopt;
        }
        found = cache.faces.emplace(cache_key, created).first;
    }
    auto face = found->second;
    std::string text_key;
    text_key.reserve(text.size() + 5);
    text_key.push_back(static_cast<char>(spec->resource));
    text_key.push_back(static_cast<char>(pixel_size & 0xffU));
    text_key.push_back(static_cast<char>((pixel_size >> 8U) & 0xffU));
    text_key.append(text);
    if (const auto rendered = cache.text.find(text_key);
            rendered != cache.text.end())
        return rendered->second;

    const auto codepoints = decode_utf8(text);
    const auto rounded_26_6 = [](const FT_Pos value) {
        return static_cast<int>(
                value >= 0 ? (value + 32) / 64 : (value - 32) / 64);
    };
    const auto floor_26_6 = [](const FT_Pos value) {
        return static_cast<int>(
                value >= 0 ? value / 64 : -((-value + 63) / 64));
    };
    const auto line_height =
            std::max(rounded_26_6(face->size->metrics.height), 1);
    int pen{};
    FT_ULong previous{};
    for (const auto codepoint : codepoints) {
        const auto glyph = FT_Get_Char_Index(face, codepoint);
        if (previous) {
            FT_Vector delta{};
            // codepoints, NOT glyph indices like FreeType wants, otherwise the
            // widths don't match
            if (FT_Get_Kerning(face, static_cast<FT_UInt>(previous),
                        static_cast<FT_UInt>(codepoint), FT_KERNING_DEFAULT,
                        &delta) == 0)
                pen += floor_26_6(delta.x);
        }
        // FT_LOAD_RENDER and floor the 26.6 advance with a shift
        if (FT_Load_Glyph(face, glyph, FT_LOAD_RENDER) == 0)
            pen += floor_26_6(face->glyph->advance.x);
        previous = codepoint;
    }
    UiRasterizedText result;
    result.width = std::max(pen, 0);
    result.height = line_height;
    result.alpha.assign(
            static_cast<std::size_t>(result.width) * result.height, 0);
    pen = 0;
    previous = 0;
    for (const auto codepoint : codepoints) {
        const auto glyph = FT_Get_Char_Index(face, codepoint);
        if (previous) {
            FT_Vector delta{};
            if (FT_Get_Kerning(face, static_cast<FT_UInt>(previous),
                        static_cast<FT_UInt>(codepoint), FT_KERNING_DEFAULT,
                        &delta) == 0)
                pen += floor_26_6(delta.x);
        }
        if (FT_Load_Glyph(face, glyph, FT_LOAD_RENDER) != 0) {
            previous = codepoint;
            continue;
        }
        const auto &bitmap = face->glyph->bitmap;
        const auto origin_x = pen + face->glyph->bitmap_left;
        // baseline from the pixel size, NOT FreeType's ascender, you can see
        // it in Standard0755J menus
        const auto origin_y =
                static_cast<int>(pixel_size) - face->glyph->bitmap_top;
        for (unsigned row = 0; row < bitmap.rows; ++row) {
            const auto y = origin_y + static_cast<int>(row);
            if (y < 0 || y >= result.height)
                continue;
            const auto *source =
                    bitmap.buffer +
                    static_cast<std::ptrdiff_t>(row) * bitmap.pitch;
            for (unsigned column = 0; column < bitmap.width; ++column) {
                const auto x = origin_x + static_cast<int>(column);
                if (x < 0 || x >= result.width)
                    continue;
                // clang-format off
                result.alpha[static_cast<std::size_t>(y) * result.width + x] =
                        std::max(result.alpha[static_cast<std::size_t>(y) *
                        result.width + x], source[column]);
                // clang-format on
            }
        }
        pen += floor_26_6(face->glyph->advance.x);
        previous = codepoint;
    }
    // values keep changing, so clear it every now and then
    if (cache.text.size() >= 1024)
        cache.text.clear();
    cache.text.emplace(std::move(text_key), result);
    return result;
}

UiRectangle intersect_rectangles(UiRectangle first, UiRectangle second);

class Painter {
  public:
    explicit Painter(UiFrame &frame)
        : target_frame(frame),
          clip_bounds{ 0, 0, static_cast<std::int32_t>(frame.width),
              static_cast<std::int32_t>(frame.height) } {}

    [[nodiscard]] UiRectangle clip() const { return clip_bounds; }
    void set_clip(UiRectangle clip) { clip_bounds = clip; }

    void rectangle(UiRectangle bounds, UiColor color) {
        const auto x0 = std::clamp(std::max(bounds.x, clip_bounds.x), 0,
                static_cast<int>(target_frame.width));
        const auto y0 = std::clamp(std::max(bounds.y, clip_bounds.y), 0,
                static_cast<int>(target_frame.height));
        const auto x1 = std::clamp(std::min(bounds.x + bounds.width,
                                           clip_bounds.x + clip_bounds.width),
                0, static_cast<int>(target_frame.width));
        const auto y1 = std::clamp(std::min(bounds.y + bounds.height,
                                           clip_bounds.y + clip_bounds.height),
                0, static_cast<int>(target_frame.height));
        if (color.alpha == 255) {
            for (int y = y0; y < y1; ++y)
                for (int x = x0; x < x1; ++x) {
                    const auto offset =
                            static_cast<std::size_t>(y) * target_frame.stride +
                            static_cast<std::size_t>(x) * 4;
                    target_frame.rgba[offset] = color.red;
                    target_frame.rgba[offset + 1] = color.green;
                    target_frame.rgba[offset + 2] = color.blue;
                    target_frame.rgba[offset + 3] = 255;
                }
            return;
        }
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x)
                pixel(x, y, color);
    }

    void outline(UiRectangle bounds, UiColor color, int thickness = 1) {
        rectangle({ bounds.x, bounds.y, bounds.width, thickness }, color);
        rectangle({ bounds.x, bounds.y + bounds.height - thickness,
                          bounds.width, thickness },
                color);
        rectangle({ bounds.x, bounds.y, thickness, bounds.height }, color);
        rectangle({ bounds.x + bounds.width - thickness, bounds.y, thickness,
                          bounds.height },
                color);
    }

    void rounded_rectangle(UiRectangle bounds, UiColor color, float radius) {
        const int inset = static_cast<int>(std::ceil(radius));
        rectangle(
                { bounds.x + inset, bounds.y,
                        std::max(bounds.width - 2 * inset, 0), bounds.height },
                color);
        for (int y = 0; y < bounds.height; ++y)
            for (int x = 0; x < std::min(inset, bounds.width); ++x) {
                const auto dx = std::max(radius - (x + 0.5F), 0.0F);
                const auto dy = std::max({ radius - (y + 0.5F),
                        y + 0.5F - (bounds.height - radius), 0.0F });
                auto edge = color;
                edge.alpha = static_cast<std::uint8_t>(
                        color.alpha *
                        std::clamp(radius + 0.5F - std::sqrt(dx * dx + dy * dy),
                                0.0F, 1.0F));
                pixel(bounds.x + x, bounds.y + y, edge);
                pixel(bounds.x + bounds.width - x - 1, bounds.y + y, edge);
            }
    }

    void line(int x0, int y0, int x1, int y1, UiColor color) {
        const int dx = std::abs(x1 - x0);
        const int sx = x0 < x1 ? 1 : -1;
        const int dy = -std::abs(y1 - y0);
        const int sy = y0 < y1 ? 1 : -1;
        int error = dx + dy;
        for (;;) {
            pixel(x0, y0, color);
            if (x0 == x1 && y0 == y1)
                break;
            const int twice = error * 2;
            if (twice >= dy) {
                error += dy;
                x0 += sx;
            }
            if (twice <= dx) {
                error += dx;
                y0 += sy;
            }
        }
    }

    void circle(int center_x, int center_y, int radius, UiColor color) {
        const int radius2 = radius * radius;
        for (int y = -radius; y <= radius; ++y)
            for (int x = -radius; x <= radius; ++x)
                if (x * x + y * y <= radius2)
                    pixel(center_x + x, center_y + y, color);
    }

    void circle_outline(int center_x, int center_y, int radius, UiColor color,
            int thickness = 1) {
        radius = std::max(radius, 1);
        thickness = std::clamp(thickness, 1, radius);
        const auto outer2 = radius * radius;
        const auto inner = radius - thickness;
        const auto inner2 = inner * inner;
        for (int y = -radius; y <= radius; ++y)
            for (int x = -radius; x <= radius; ++x) {
                const auto distance2 = x * x + y * y;
                if (distance2 <= outer2 && distance2 >= inner2)
                    pixel(center_x + x, center_y + y, color);
            }
    }

    void image(const UiImage &source, UiRectangle destination,
            std::int32_t animation_state = 0,
            const float fixed_segment_scale = 1.0F) {
        if (!source.valid() || destination.width <= 0 ||
                destination.height <= 0)
            return;
        const auto frames = std::max(source.animation_count, 1U);
        const auto frame = static_cast<std::uint32_t>(std::clamp(
                animation_state, 0, static_cast<std::int32_t>(frames - 1)));
        const auto source_width = source.horizontal_animation
                                          ? std::max(source.width / frames, 1U)
                                          : source.width;
        const auto source_height =
                source.horizontal_animation
                        ? source.height
                        : std::max(source.height / frames, 1U);
        const auto source_x =
                source.horizontal_animation ? frame * source_width : 0U;
        const auto source_y =
                source.horizontal_animation ? 0U : frame * source_height;
        const UiRectangle source_frame{ static_cast<std::int32_t>(source_x),
            static_cast<std::int32_t>(source_y),
            static_cast<std::int32_t>(source_width),
            static_cast<std::int32_t>(source_height) };
        if (!source.horizontal_resizable && !source.vertical_resizable) {
            image_region(source, destination, source_frame);
            return;
        }

        const auto left =
                static_cast<int>(std::min(source.fixed_left, source_width));
        const auto right = static_cast<int>(std::min(source.fixed_right,
                source_width - static_cast<std::uint32_t>(left)));
        const auto top =
                static_cast<int>(std::min(source.fixed_top, source_height));
        const auto bottom = static_cast<int>(std::min(source.fixed_bottom,
                source_height - static_cast<std::uint32_t>(top)));
        const std::array source_xs{ source_frame.x, source_frame.x + left,
            source_frame.x + source_frame.width - right,
            source_frame.x + source_frame.width };
        const std::array source_ys{ source_frame.y, source_frame.y + top,
            source_frame.y + source_frame.height - bottom,
            source_frame.y + source_frame.height };
        const auto scaled_fixed = [&](const int value) {
            return static_cast<int>(
                    std::lround(value * std::max(fixed_segment_scale, 0.0F)));
        };
        const auto destination_left =
                std::min(scaled_fixed(left), destination.width);
        const auto destination_right = std::min(
                scaled_fixed(right), destination.width - destination_left);
        const auto destination_top =
                std::min(scaled_fixed(top), destination.height);
        const auto destination_bottom = std::min(
                scaled_fixed(bottom), destination.height - destination_top);
        const std::array destination_xs{ destination.x,
            destination.x + destination_left,
            destination.x + destination.width - destination_right,
            destination.x + destination.width };
        const std::array destination_ys{ destination.y,
            destination.y + destination_top,
            destination.y + destination.height - destination_bottom,
            destination.y + destination.height };
        for (std::size_t row = 0; row < 3; ++row) {
            for (std::size_t column = 0; column < 3; ++column) {
                const UiRectangle source_piece{ source_xs[column],
                    source_ys[row], source_xs[column + 1] - source_xs[column],
                    source_ys[row + 1] - source_ys[row] };
                const UiRectangle destination_piece{ destination_xs[column],
                    destination_ys[row],
                    destination_xs[column + 1] - destination_xs[column],
                    destination_ys[row + 1] - destination_ys[row] };
                // tile, don't stretch, partial tiles included
                if (source_piece.width <= 0 || source_piece.height <= 0 ||
                        destination_piece.width <= 0 ||
                        destination_piece.height <= 0)
                    continue;
                const auto tile_width =
                        std::max(scaled_fixed(source_piece.width), 1);
                const auto tile_height =
                        std::max(scaled_fixed(source_piece.height), 1);
                const auto previous_clip = clip();
                set_clip(
                        intersect_rectangles(previous_clip, destination_piece));
                for (int y = destination_piece.y;
                        y < destination_piece.y + destination_piece.height;
                        y += tile_height)
                    for (int x = destination_piece.x;
                            x < destination_piece.x + destination_piece.width;
                            x += tile_width)
                        image_region(source, { x, y, tile_width, tile_height },
                                source_piece);
                set_clip(previous_clip);
            }
        }
    }

    void image_region(const UiImage &source, UiRectangle destination,
            UiRectangle region, const bool raw_pixels = false) {
        if (!source.valid() || destination.width <= 0 ||
                destination.height <= 0 || region.width <= 0 ||
                region.height <= 0)
            return;
        const auto source_x = static_cast<std::uint32_t>(
                std::clamp(region.x, 0, static_cast<int>(source.width - 1)));
        const auto source_y = static_cast<std::uint32_t>(
                std::clamp(region.y, 0, static_cast<int>(source.height - 1)));
        const auto source_width = std::min<std::uint32_t>(
                static_cast<std::uint32_t>(region.width),
                source.width - source_x);
        const auto source_height = std::min<std::uint32_t>(
                static_cast<std::uint32_t>(region.height),
                source.height - source_y);
        const bool exact =
                destination.width == static_cast<int>(source_width) &&
                destination.height == static_cast<int>(source_height);
        const auto visible = intersect_rectangles(
                intersect_rectangles(destination, clip_bounds),
                { 0, 0, static_cast<int>(target_frame.width),
                        static_cast<int>(target_frame.height) });
        if (visible.width <= 0 || visible.height <= 0)
            return;
        constexpr std::size_t maximum_cached_bytes = 16 * 1024 * 1024;
        const auto raster_bytes = static_cast<std::size_t>(destination.width) *
                                  destination.height * 4;
        if (!raw_pixels && !exact && raster_bytes >= 4096 &&
                raster_bytes <= maximum_cached_bytes && source.scale_cache) {
            const auto &cache = source.scale_cache;
            std::shared_ptr<const UiImage> raster;
            {
                std::scoped_lock lock(cache->mutex);
                // the pixels can change under us, drop the cache then
                if (cache->source_width != source.width ||
                        cache->source_height != source.height ||
                        cache->source_pixels != source.rgba) {
                    cache->source_pixels = source.rgba;
                    cache->source_width = source.width;
                    cache->source_height = source.height;
                    cache->entries.clear();
                    cache->bytes = 0;
                }
                const std::array geometry{ static_cast<int>(source_x),
                    static_cast<int>(source_y), static_cast<int>(source_width),
                    static_cast<int>(source_height), destination.width,
                    destination.height };
                for (const auto &entry : cache->entries)
                    if (entry.geometry == geometry) {
                        raster = entry.image;
                        break;
                    }
                if (!raster) {
                    UiFrame filtered;
                    filtered.width = destination.width;
                    filtered.height = destination.height;
                    filtered.stride = filtered.width * 4;
                    filtered.rgba.resize(raster_bytes);
                    Painter{ filtered }.image_region(source,
                            { 0, 0, destination.width, destination.height },
                            region, true);
                    auto image = std::make_shared<UiImage>();
                    image->width = filtered.width;
                    image->height = filtered.height;
                    image->rgba = std::move(filtered.rgba);
                    image->opaque = true;
                    for (std::size_t i = 3; i < image->rgba.size(); i += 4)
                        if (image->rgba[i] != 255) {
                            image->opaque = false;
                            break;
                        }
                    raster = std::move(image);
                    if (cache->bytes + raster_bytes > maximum_cached_bytes ||
                            cache->entries.size() >= 8) {
                        cache->entries.clear();
                        cache->bytes = 0;
                    }
                    cache->entries.push_back({ geometry, raster });
                    cache->bytes += raster_bytes;
                }
            }
            image_region(*raster, destination,
                    { 0, 0, destination.width, destination.height });
            return;
        }
        if (exact && (source.opaque || raw_pixels)) {
            for (int y = visible.y; y < visible.y + visible.height; ++y) {
                // clang-format off
                const auto source_offset = (static_cast<std::size_t>(source_y +
                        y - destination.y) * source.width + source_x +
                        visible.x - destination.x) * 4;
                // clang-format on
                const auto target_offset =
                        static_cast<std::size_t>(y) * target_frame.stride +
                        visible.x * 4;
                std::memcpy(target_frame.rgba.data() + target_offset,
                        source.rgba.data() + source_offset,
                        static_cast<std::size_t>(visible.width) * 4);
            }
            return;
        }
        for (int y = visible.y - destination.y;
                y < visible.y - destination.y + visible.height; ++y) {
            for (int x = visible.x - destination.x;
                    x < visible.x - destination.x + visible.width; ++x) {
                if (exact) {
                    // no interpolation, but pixel() still does the alpha
                    // clang-format off
                    const auto offset = (static_cast<std::size_t>(source_y +
                            y) * source.width + source_x + x) * 4;
                    // clang-format on
                    pixel(destination.x + x, destination.y + y,
                            { source.rgba[offset], source.rgba[offset + 1],
                                    source.rgba[offset + 2],
                                    source.rgba[offset + 3] });
                    continue;
                }
                const auto sample_x =
                        source_x +
                        (x + 0.5) * source_width / destination.width - 0.5;
                const auto sample_y =
                        source_y +
                        (y + 0.5) * source_height / destination.height - 0.5;
                const auto clamped_x = std::clamp(sample_x,
                        static_cast<double>(source_x),
                        static_cast<double>(source_x + source_width - 1));
                const auto clamped_y = std::clamp(sample_y,
                        static_cast<double>(source_y),
                        static_cast<double>(source_y + source_height - 1));
                const auto sx0 =
                        static_cast<std::uint32_t>(std::floor(clamped_x));
                const auto sy0 =
                        static_cast<std::uint32_t>(std::floor(clamped_y));
                const auto sx1 = std::min(sx0 + 1, source_x + source_width - 1);
                const auto sy1 =
                        std::min(sy0 + 1, source_y + source_height - 1);
                const auto fx = clamped_x - sx0;
                const auto fy = clamped_y - sy0;
                double alpha{};
                double rgb[3]{};
                const std::array samples{ std::tuple{ sx0, sy0,
                                                  (1.0 - fx) * (1.0 - fy) },
                    std::tuple{ sx1, sy0, fx * (1.0 - fy) },
                    std::tuple{ sx0, sy1, (1.0 - fx) * fy },
                    std::tuple{ sx1, sy1, fx * fy } };
                for (const auto [sx, sy, weight] : samples) {
                    const auto offset =
                            (static_cast<std::size_t>(sy) * source.width + sx) *
                            4;
                    const auto a = source.rgba[offset + 3] / 255.0;
                    alpha += a * weight;
                    for (std::size_t channel = 0; channel < 3; ++channel)
                        rgb[channel] +=
                                source.rgba[offset + channel] * a * weight;
                }
                std::array<std::uint8_t, 3> channels{};
                for (std::size_t channel = 0; channel < 3; ++channel)
                    channels[channel] = static_cast<std::uint8_t>(std::clamp(
                            std::lround(
                                    alpha > 0.0 ? rgb[channel] / alpha : 0.0),
                            0L, 255L));
                const UiColor sampled{ channels[0], channels[1], channels[2],
                    static_cast<std::uint8_t>(
                            std::clamp(std::lround(alpha * 255.0), 0L, 255L)) };
                if (raw_pixels) {
                    const auto offset =
                            static_cast<std::size_t>(destination.y + y) *
                                    target_frame.stride +
                            (destination.x + x) * 4;
                    target_frame.rgba[offset] = sampled.red;
                    target_frame.rgba[offset + 1] = sampled.green;
                    target_frame.rgba[offset + 2] = sampled.blue;
                    target_frame.rgba[offset + 3] = sampled.alpha;
                } else
                    pixel(destination.x + x, destination.y + y, sampled);
            }
        }
    }

    void text(int x, int y, std::string_view value, UiColor color) {
        for (const unsigned char character : value) {
            const auto glyph = ui_fallback_glyph_rows(character);
            for (int row = 0; row < 7; ++row)
                for (int column = 0; column < 5; ++column)
                    if ((glyph[row] >> (4 - column)) & 1U)
                        rectangle({ x + column, y + row, 1, 1 }, color);
            x += 6;
        }
    }

    void text(int x, int y, const UiRasterizedText &value, UiColor color) {
        for (int row = 0; row < value.height; ++row) {
            for (int column = 0; column < value.width; ++column) {
                const auto coverage =
                        value.alpha[static_cast<std::size_t>(row) *
                                            value.width +
                                    column];
                if (!coverage)
                    continue;
                auto covered = color;
                covered.alpha = static_cast<std::uint8_t>(
                        static_cast<unsigned>(color.alpha) * coverage / 255U);
                pixel(x + column, y + row, covered);
            }
        }
    }

  private:
    void pixel(int x, int y, UiColor source) {
        if (x < 0 || y < 0 || x >= static_cast<int>(target_frame.width) ||
                y >= static_cast<int>(target_frame.height) ||
                !clip_bounds.contains(x, y))
            return;
        const auto offset = static_cast<std::size_t>(y) * target_frame.stride +
                            static_cast<std::size_t>(x) * 4;
        const unsigned alpha = source.alpha;
        const unsigned inverse = 255U - alpha;
        target_frame.rgba[offset] = static_cast<std::uint8_t>(
                (source.red * alpha + target_frame.rgba[offset] * inverse) /
                255U);
        target_frame.rgba[offset + 1] = static_cast<std::uint8_t>(
                (source.green * alpha +
                        target_frame.rgba[offset + 1] * inverse) /
                255U);
        target_frame.rgba[offset + 2] = static_cast<std::uint8_t>(
                (source.blue * alpha +
                        target_frame.rgba[offset + 2] * inverse) /
                255U);
        target_frame.rgba[offset + 3] = 255;
    }

    UiFrame &target_frame;
    UiRectangle clip_bounds;
};

UiRectangle intersect_rectangles(
        const UiRectangle first, const UiRectangle second) {
    const auto left = std::max(first.x, second.x);
    const auto top = std::max(first.y, second.y);
    const auto right = std::min(first.x + first.width, second.x + second.width);
    const auto bottom =
            std::min(first.y + first.height, second.y + second.height);
    return { left, top, std::max(right - left, 0), std::max(bottom - top, 0) };
}

int centered_text_offset(const int available, const int required) {
    auto remainder = available - required;
    if (remainder < 0)
        ++remainder;
    return remainder >> 1;
}

std::int32_t selected_font_type(const UiControl &control, const bool on) {
    const std::int32_t *state_type = nullptr;
    if (control.pressed)
        state_type = on ? &control.font_type_on_pressed
                        : &control.font_type_off_pressed;
    else if (control.hovered)
        state_type =
                on ? &control.font_type_on_hover : &control.font_type_off_hover;
    else if (on)
        state_type = &control.font_type_on;
    return state_type && *state_type != 0 ? *state_type : control.font_type;
}

const std::string &selected_font_name(const UiControl &control, const bool on) {
    const std::string *state_name = nullptr;
    if (control.pressed)
        state_name = on ? &control.font_name_on_pressed
                        : &control.font_name_off_pressed;
    else if (control.hovered)
        state_name =
                on ? &control.font_name_on_hover : &control.font_name_off_hover;
    else if (on)
        state_name = &control.font_name_on;
    return state_name && !state_name->empty() ? *state_name : control.font_name;
}

struct PictureFontLayout {
    std::shared_ptr<const UiImage> image;
    std::array<std::int32_t, 256> source_x{};
    std::array<std::int32_t, 256> widths{};
    std::int32_t source_y{};
    std::int32_t height{};
    float render_scale{ 1.0F };
};

std::optional<PictureFontLayout> picture_font_layout(
        const UiAssetProvider *assets, const std::string_view name,
        const float pixel_density) {
    if (!assets || name.empty())
        return std::nullopt;
    auto image = assets->image(name, pixel_density);
    if (!image || !image->valid() || image->height == 0)
        return std::nullopt;
    PictureFontLayout result;
    result.image = std::move(image);
    result.render_scale =
            pixel_density / std::max(result.image->pixel_density, 1.0F);
    // exactly 256 non-zero pixels on the first row means it's the marker row,
    // and it isn't part of the glyphs
    constexpr std::uint32_t metadata_y = 0;
    std::size_t markers{};
    const auto marked = [&](const std::uint32_t x) {
        const auto offset =
                (static_cast<std::size_t>(metadata_y) * result.image->width +
                        x) *
                4;
        return result.image->rgba[offset] != 0 ||
               result.image->rgba[offset + 1] != 0 ||
               result.image->rgba[offset + 2] != 0 ||
               result.image->rgba[offset + 3] != 0;
    };
    for (std::uint32_t x = 0; x < result.image->width; ++x)
        markers += marked(x);
    if (markers != 256) {
        // plain 256 cell strip. it's a ceiling, flooring shifts every glyph
        // when the width doesn't divide
        const auto width =
                static_cast<std::int32_t>((result.image->width + 255U) / 256U);
        if (width <= 0)
            return std::nullopt;
        result.height = static_cast<std::int32_t>(result.image->height);
        for (std::size_t character = 0; character < 256; ++character) {
            result.source_x[character] =
                    static_cast<std::int32_t>(character) * width;
            result.widths[character] = width;
        }
        return result;
    }
    result.source_y = 1;
    result.height = static_cast<std::int32_t>(result.image->height) - 1;
    std::array<std::int32_t, 256> marker_positions{};
    std::size_t marker_index{};
    for (std::uint32_t x = 0; x < result.image->width; ++x)
        if (marked(x))
            marker_positions[marker_index++] = static_cast<std::int32_t>(x);
    for (std::size_t character = 0; character < 256; ++character) {
        // the markers are already in atlas coordinates
        result.source_x[character] = marker_positions[character];
        // clang-format off
        result.widths[character] = character + 1 < marker_positions.size() ?
                std::max(marker_positions[character + 1] -
                marker_positions[character], 1) :
                std::max(static_cast<std::int32_t>(result.image->width) -
                marker_positions[character], 1);
        // clang-format on
    }
    return result;
}

std::int32_t picture_font_width(
        const PictureFontLayout &font, const std::string_view text) {
    std::int32_t width{};
    for (const auto codepoint : decode_utf8(text, true))
        width += font.widths[codepoint < 256 ? codepoint : 0x3f];
    return static_cast<std::int32_t>(std::lround(width * font.render_scale));
}

void paint_picture_font_line(Painter &painter, const PictureFontLayout &font,
        const std::string_view text, std::int32_t x, const std::int32_t y) {
    for (const auto codepoint : decode_utf8(text, true)) {
        const auto character =
                static_cast<std::size_t>(codepoint < 256 ? codepoint : 0x3f);
        const auto source_width = font.widths[character];
        const auto width = static_cast<std::int32_t>(
                std::lround(source_width * font.render_scale));
        const auto height = static_cast<std::int32_t>(
                std::lround(font.height * font.render_scale));
        painter.image_region(*font.image, { x, y, width, height },
                { font.source_x[character], font.source_y, source_width,
                        font.height });
        x += width;
    }
}

void paint_authored_text(Painter &painter, const UiControl &control,
        const UiAssetProvider *assets, const float pixel_density,
        const bool native_ui) {
    auto text = control.text;
    if (text.empty() && control.kind == UiControlKind::text_edit) {
        if (const auto *value = std::get_if<std::string>(&control.value))
            text = *value;
    }
    if (control.kind == UiControlKind::menu) {
        // the selected entry wins over the caption, saved ones often still
        // have the menu's internal name
        const auto value = integer_value(control.value);
        const auto found = std::ranges::find_if(control.menu_entries,
                [&](const auto &entry) { return entry.first == value; });
        if (found != control.menu_entries.end())
            text = found->second;
    }
    // keep the whole string, leading spaces are padding
    if (text.empty())
        return;
    const auto previous_clip = painter.clip();
    auto text_bounds = control.bounds;
    if (!native_ui && control.kind == UiControlKind::text_edit) {
        // only the 1px top border stays, still top aligned
        const auto top = static_cast<int>(std::lround(pixel_density));
        text_bounds.y += top;
        text_bounds.height = std::max(text_bounds.height - top, 0);
    }
    if (!native_ui &&
            (control.kind == UiControlKind::menu ||
                    control.kind == UiControlKind::button ||
                    control.kind == UiControlKind::switch_control) &&
            assets && !control.picture.empty()) {
        // resize the switch itself, not just the bitmap
        if (const auto skin = assets->image(control.picture, pixel_density)) {
            const auto density = std::max(skin->pixel_density, 1.0F);
            const auto frames = std::max(skin->animation_count, 1U);
            const auto frame_width =
                    skin->width / (skin->horizontal_animation ? frames : 1U);
            const auto frame_height =
                    skin->height / (skin->horizontal_animation ? 1U : frames);
            if (!skin->horizontal_resizable)
                text_bounds.width =
                        static_cast<int>(std::lround(
                                frame_width * pixel_density / density)) +
                        2 * static_cast<int>(std::lround(pixel_density));
            if (!skin->vertical_resizable)
                text_bounds.height =
                        static_cast<int>(std::lround(
                                frame_height * pixel_density / density)) +
                        2 * static_cast<int>(std::lround(pixel_density));
        }
    }
    if (!native_ui && control.kind == UiControlKind::label) {
        // text at left+7, centered and right also get 5px on the right
        const auto left = static_cast<int>(std::lround(7 * pixel_density));
        const auto right = static_cast<int>(std::lround(
                (control.text_alignment == 0 ? -5 : 5) * pixel_density));
        text_bounds.x += left;
        text_bounds.width = std::max(text_bounds.width - left - right, 0);
        // at least 15px at the bottom even if TEXTPOS_Y says less
        const auto minimum = static_cast<int>(std::lround(15 * pixel_density));
        const auto requested = static_cast<int>(
                std::lround(control.text_position_y * pixel_density));
        const auto top = std::min(requested, text_bounds.height - minimum);
        text_bounds.y += top + static_cast<int>(std::lround(pixel_density));
        text_bounds.height = std::max(
                text_bounds.height - top -
                        static_cast<int>(std::lround(3 * pixel_density)),
                0);
    }
    const bool classic_text_panel =
            !native_ui &&
            (control.kind == UiControlKind::button ||
                    control.kind == UiControlKind::switch_control ||
                    control.kind == UiControlKind::menu);
    if (classic_text_panel) {
        // one pixel inside the rectangle. script buttons get 6px on BOTH sides,
        // normal ones only on the aligned side, menus are left=6 right=0
        // bottom=2
        const auto scaled = [&](int value) {
            return static_cast<int>(std::lround(value * pixel_density));
        };
        const auto inset = scaled(1);
        const auto left = scaled(control.kind != UiControlKind::button ||
                                                 control.text_alignment == 0
                                         ? 6
                                         : 0);
        const auto right = scaled(
                control.kind == UiControlKind::switch_control ||
                                (control.kind == UiControlKind::button &&
                                        control.text_alignment == 2)
                        ? 6
                        : 0);
        const auto top = scaled(control.text_position_y - 1);
        const auto bottom = scaled(control.kind == UiControlKind::menu ? 2 : 0);
        text_bounds.x += inset + left;
        text_bounds.y += inset + top;
        text_bounds.width =
                std::max(text_bounds.width - 2 * inset - left - right, 0);
        text_bounds.height =
                std::max(text_bounds.height - 2 * inset - top - bottom, 0);
        painter.set_clip(intersect_rectangles(previous_clip, text_bounds));
    } else if (control.kind == UiControlKind::menu && assets &&
               !control.picture.empty()) {
        if (const auto skin = assets->image(control.picture)) {
            // text goes in the stretchy middle of the nine-slice and gets
            // clipped there, an arrow-only skin has basically no middle so
            // nothing leaks over the label next to it
            const auto margin_scale =
                    pixel_density / std::max(skin->pixel_density, 1.0F);
            const auto left = static_cast<std::int32_t>(
                    std::lround(skin->fixed_left * margin_scale));
            const auto right = static_cast<std::int32_t>(
                    std::lround(skin->fixed_right * margin_scale));
            const auto top = static_cast<std::int32_t>(
                    std::lround(skin->fixed_top * margin_scale));
            const auto bottom = static_cast<std::int32_t>(
                    std::lround(skin->fixed_bottom * margin_scale));
            text_bounds.x += left;
            text_bounds.y += top;
            text_bounds.width = std::max(text_bounds.width - left - right, 0);
            text_bounds.height = std::max(text_bounds.height - top - bottom, 0);
            painter.set_clip(intersect_rectangles(previous_clip, text_bounds));
        }
    }
    std::vector<std::string_view> lines;
    for (std::size_t start = 0;;) {
        const auto end = text.find('\n', start);
        lines.emplace_back(text.data() + start,
                (end == std::string::npos ? text.size() : end) - start);
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    const auto font_type =
            selected_font_type(control, integer_value(control.value) != 0);
    const auto picture_font = picture_font_layout(assets,
            selected_font_name(control, integer_value(control.value) != 0),
            pixel_density);
    std::vector<std::optional<UiRasterizedText>> rasters;
    rasters.reserve(lines.size());
    int text_height{};
    for (const auto line : lines) {
        rasters.push_back(picture_font ? std::nullopt
                                       : rasterize_builtin_text(line, font_type,
                                                 pixel_density));
        text_height += picture_font     ? static_cast<int>(std::lround(
                                              picture_font->height *
                                              picture_font->render_scale))
                       : rasters.back() ? rasters.back()->height
                                        : 8;
    }
    auto text_top = text_bounds.y;
    auto text_height_available = text_bounds.height;
    if (!classic_text_panel && control.kind == UiControlKind::switch_control) {
        // TEXTPOS_Y - 1 ends up as the top border, so it shrinks the rectangle
        const auto top_border = static_cast<int>(
                std::lround((control.text_position_y - 1) * pixel_density));
        text_top += top_border;
        text_height_available -= top_border;
    }
    // top aligned unless the mode is explicitly 1
    const auto top_align_oversized_picture_font =
            picture_font && text_height > text_height_available &&
            !classic_text_panel;
    const auto y =
            text_top +
            (classic_text_panel || control.kind == UiControlKind::label ||
                                    (!native_ui &&
                                            control.kind ==
                                                    UiControlKind::text_edit) ||
                                    top_align_oversized_picture_font
                            ? 0
                            : centered_text_offset(
                                      text_height_available, text_height));
    int line_y = y;
    for (std::size_t index = 0; index < lines.size(); ++index) {
        const auto text_width =
                picture_font ? picture_font_width(*picture_font, lines[index])
                : rasters[index]
                        ? rasters[index]->width
                        : std::max(
                                  static_cast<int>(lines[index].size()) * 6 - 1,
                                  0);
        // no room between the borders means no text at all, NOT half of it
        if (control.kind == UiControlKind::menu &&
                text_width > text_bounds.width)
            continue;
        int x = text_bounds.x;
        if (control.text_alignment == 1) {
            // center in logical pixels BEFORE scaling
            if (classic_text_panel && picture_font)
                // clang-format off
                x +=
                        static_cast<int>(std::lround(centered_text_offset(
                        static_cast<int>(std::lround(text_bounds.width /
                        pixel_density)),
                        static_cast<int>(std::lround(text_width /
                        pixel_density))) * pixel_density));
            // clang-format on
            else
                x += centered_text_offset(text_bounds.width, text_width);
        } else if (control.text_alignment == 2)
            x += text_bounds.width - text_width;
        if (picture_font)
            paint_picture_font_line(
                    painter, *picture_font, lines[index], x, line_y);
        else if (rasters[index])
            painter.text(x, line_y, *rasters[index], control.text_color);
        else
            painter.text(x, line_y, lines[index], control.text_color);
        line_y += picture_font     ? static_cast<int>(std::lround(
                                         picture_font->height *
                                         picture_font->render_scale))
                  : rasters[index] ? rasters[index]->height
                                   : 8;
    }
    painter.set_clip(previous_clip);
}

UiColor contrasting_text_color(
        const UiImage &image, std::int32_t state, UiColor fallback) {
    const auto frames = std::max(image.animation_count, 1U);
    const auto frame = static_cast<std::uint32_t>(
            std::clamp(state, 0, static_cast<std::int32_t>(frames - 1)));
    const auto width =
            image.horizontal_animation ? image.width / frames : image.width;
    const auto height =
            image.horizontal_animation ? image.height : image.height / frames;
    const auto origin_x = image.horizontal_animation ? frame * width : 0U;
    const auto origin_y = image.horizontal_animation ? 0U : frame * height;
    std::uint64_t luminance{};
    std::uint64_t weight{};
    for (std::uint32_t y = 0; y < height; y += std::max(height / 8U, 1U)) {
        for (std::uint32_t x = 0; x < width; x += std::max(width / 8U, 1U)) {
            const auto offset =
                    (static_cast<std::size_t>(origin_y + y) * image.width +
                            origin_x + x) *
                    4;
            const auto alpha = image.rgba[offset + 3];
            luminance += alpha * (299U * image.rgba[offset] +
                                         587U * image.rgba[offset + 1] +
                                         114U * image.rgba[offset + 2]);
            weight += alpha * 1000U;
        }
    }
    if (weight && luminance / weight > 155U)
        return { 42, 42, 45, 255 };
    return fallback;
}

std::optional<UiColor> declared_font_color(
        const UiControl &control, const bool on) {
    const std::string *state_name = nullptr;
    if (control.pressed) {
        state_name = on ? &control.font_name_on_pressed
                        : &control.font_name_off_pressed;
    } else if (control.hovered) {
        state_name =
                on ? &control.font_name_on_hover : &control.font_name_off_hover;
    } else if (on) {
        state_name = &control.font_name_on;
    }
    const auto &name = state_name && !state_name->empty() ? *state_name
                                                          : control.font_name;
    if (!name.empty()) {
        std::string folded = name;
        std::ranges::transform(folded, folded.begin(), [](unsigned char value) {
            return static_cast<char>(std::tolower(value));
        });
        const bool dark = folded.find("dark") != std::string::npos ||
                          folded.find("black") != std::string::npos;
        const bool translucent = folded.find("trans") != std::string::npos;
        return dark ? std::optional<UiColor>{ UiColor{ 42, 42, 45,
                              static_cast<std::uint8_t>(
                                      translucent ? 110 : 255) } }
                    : std::optional<UiColor>{ UiColor{ 235, 235, 235,
                              static_cast<std::uint8_t>(
                                      translucent ? 110 : 255) } };
    }
    // unset state fonts fall back to the off one
    const auto font_type = selected_font_type(control, on);
    // one-based KSP font types
    switch (font_type) {
        case 1:
            return UiColor{ 255, 255, 255, 255 }; // NGL 95
        case 2:
            return UiColor{ 51, 55, 55, 255 }; // NGL 96
        case 3:
            return UiColor{ 205, 205, 205, 255 }; // NGL 97
        case 4:
            return UiColor{ 106, 195, 228, 255 }; // NGL 106
        case 5:
            return UiColor{ 131, 210, 229, 255 }; // NGL 103
        case 6:
            return UiColor{ 19, 49, 126, 255 }; // NGL 107
        case 7:
            return UiColor{ 26, 54, 134, 255 }; // NGL 104
        case 8:
            return UiColor{ 61, 69, 73, 255 }; // NGL 105
        case 9:
            return UiColor{ 0, 0, 0, 255 }; // NGL 110
        case 10:
            return UiColor{ 218, 218, 218, 255 }; // NGL 108
        case 11:
            return UiColor{ 142, 141, 138, 255 }; // NGL 109
        case 12:
            return UiColor{ 26, 27, 27, 255 }; // NGL 38
        case 13:
            return UiColor{ 226, 221, 211, 255 }; // NGL 111
        case 14:
            return UiColor{ 186, 186, 186, 255 }; // NGL 112
        case 15:
            return UiColor{ 96, 96, 96, 255 }; // NGL 115
        case 16:
            return UiColor{ 0, 0, 0, 255 }; // NGL 113
        case 17:
            return UiColor{ 255, 255, 255, 255 }; // NGL 114
        case 18:
            return UiColor{ 255, 255, 255, 255 }; // NGL 116
        case 19:
            return UiColor{ 0, 0, 0, 255 }; // NGL 117
        case 20:
            return UiColor{ 128, 128, 128, 255 }; // NGL 118
        case 21:
            return UiColor{ 128, 128, 128, 255 }; // NGL 119
        case 22:
            return UiColor{ 0, 0, 0, 255 }; // NGL 120
        case 23:
            return UiColor{ 128, 128, 128, 255 }; // NGL 121
        case 24:
            return UiColor{ 255, 255, 255, 255 }; // NGL 122
        case 25:
            return UiColor{ 49, 36, 12, 255 }; // NGL 123
        default:
            break;
    }
    return std::nullopt;
}

UiColor authored_font_color(
        const UiControl &control, const bool on, const UiImage &image) {
    if (const auto color = declared_font_color(control, on))
        return *color;
    return contrasting_text_color(image, on ? 1 : 0, control.text_color);
}

void paint_authored_waveform(Painter &painter, const UiControl &control) {
    if (control.waveform.empty() || control.bounds.width <= 0 ||
            control.bounds.height <= 0)
        return;
    if (control.waveform_background_color.alpha != 0)
        painter.rectangle(control.bounds, control.waveform_background_color);
    const auto center = control.bounds.y + control.bounds.height / 2;
    const auto amplitude = std::max(control.bounds.height / 2 - 1, 1);
    for (int x = 0; x < control.bounds.width; ++x) {
        const auto index = std::min<std::size_t>(
                static_cast<std::size_t>(x) * control.waveform.size() /
                        static_cast<std::size_t>(control.bounds.width),
                control.waveform.size() - 1);
        const auto [minimum, maximum] = control.waveform[index];
        const auto y0 =
                center - static_cast<int>(std::lround(
                                 std::clamp(maximum, -1.0F, 1.0F) * amplitude));
        const auto y1 =
                center - static_cast<int>(std::lround(
                                 std::clamp(minimum, -1.0F, 1.0F) * amplitude));
        painter.line(control.bounds.x + x, std::min(y0, y1),
                control.bounds.x + x, std::max(y0, y1), control.waveform_color);
    }
}

void paint_authored_table(Painter &painter, const UiControl &control) {
    const auto *values = std::get_if<std::vector<std::int64_t>>(&control.value);
    if (!values || values->empty() || control.bounds.width <= 0 ||
            control.bounds.height <= 0)
        return;
    if (control.table_background_color.alpha != 0)
        painter.rectangle(control.bounds, control.table_background_color);
    const auto zero = control.table_bipolar
                              ? control.bounds.y + control.bounds.height / 2
                              : control.bounds.y + control.bounds.height;
    if (control.table_bipolar)
        painter.line(control.bounds.x, zero,
                control.bounds.x + control.bounds.width - 1, zero,
                control.table_zero_color);
    for (std::size_t index = 0; index < values->size(); ++index) {
        const auto x0 =
                control.bounds.x +
                static_cast<int>(
                        index * static_cast<std::size_t>(control.bounds.width) /
                        values->size());
        const auto x1 = control.bounds.x +
                        static_cast<int>(
                                (index + 1) *
                                static_cast<std::size_t>(control.bounds.width) /
                                values->size());
        const auto normalized = std::clamp(
                static_cast<double>((*values)[index]) / control.table_maximum,
                control.table_bipolar ? -1.0 : 0.0, 1.0);
        const auto height = static_cast<int>(
                std::lround(normalized *
                            (control.table_bipolar ? control.bounds.height / 2.0
                                                   : control.bounds.height)));
        const auto top = std::min(zero, zero - height);
        painter.rectangle({ x0, top, std::max(x1 - x0 - 1, 1),
                                  std::max(std::abs(height), 1) },
                control.table_bar_color);
    }
}

void paint_xy_control(Painter &painter, const UiControl &control,
        const UiAssetProvider *assets, const float scale) {
    const UiColor fallback_cursor{ 0, 0, 0, 255 };
    const auto *values = std::get_if<std::vector<double>>(&control.value);
    if (!values)
        return;
    for (std::size_t axis = 0; axis + 1 < values->size(); axis += 2) {
        // clang-format off
        const auto center_x = control.bounds.x +
                static_cast<int>(std::lround(std::clamp((*values)[axis], 0.0,
                1.0) * std::max(control.bounds.width - 1, 0)));
        // clang-format on
        const auto center_y =
                control.bounds.y +
                static_cast<int>(std::lround(
                        (1.0 - std::clamp((*values)[axis + 1], 0.0, 1.0)) *
                        std::max(control.bounds.height - 1, 0)));
        const auto cursor = axis / 2;
        const auto picture_index = cursor * 2;
        std::shared_ptr<const UiImage> image;
        if (assets && picture_index < control.xy_cursor_pictures.size() &&
                !control.xy_cursor_pictures[picture_index].empty())
            image = assets->image(
                    control.xy_cursor_pictures[picture_index], scale);
        if (image && image->valid()) {
            const auto bounds =
                    ui_xy_cursor_bounds(control, cursor, assets, scale);
            const auto active =
                    static_cast<int>(cursor) == control.xy_active_index;
            const auto hovered =
                    control.xy_hover_index == static_cast<int>(cursor);
            // banks are idle, clicked, hotspot, NOT idle/hover/click. hotspot
            // on the active cursor, and on all of them in mode 0
            const bool hotspot = active || control.xy_mouse_mode == 0;
            const auto bank_size = std::max<int>(
                    image->animation_count / (hotspot ? 3 : 2), 1);
            // clang-format off
            const int state = std::min(active ? 1 : 0, bank_size - 1) +
                    (control.pressed && active ? bank_size : hovered &&
                    hotspot ? bank_size * 2 : 0);
            // clang-format on
            if (bounds)
                painter.image(*image, *bounds, state);
        } else {
            // without CONTROL_PAR_CURSOR_PICTURE it's a hollow dark ring
            const auto active =
                    static_cast<int>(cursor) == control.xy_active_index;
            const auto radius = std::max(
                    static_cast<int>(
                            std::lround((active ? 9.0F : 8.0F) * scale)),
                    1);
            const auto thickness =
                    std::max(static_cast<int>(std::lround(scale)), 1);
            painter.circle_outline(
                    center_x, center_y, radius, fallback_cursor, thickness);
        }
    }
}

float normalized_value(const UiControl &control) {
    const auto value = integer_value(control.value);
    if (control.maximum <= control.minimum)
        return value != 0 ? 1.0F : 0.0F;
    return std::clamp(
            static_cast<float>(value - control.minimum) /
                    static_cast<float>(control.maximum - control.minimum),
            0.0F, 1.0F);
}

std::string canonical_name(std::string_view name) {
    std::string result(name);
    std::ranges::transform(result, result.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    return result;
}

using ControlIndex = std::unordered_map<std::string, const UiControl *>;

ControlIndex control_index(const UiDocument &document) {
    ControlIndex result;
    result.reserve(document.controls.size());
    for (const auto &control : document.controls)
        result.emplace(canonical_name(control.name), &control);
    return result;
}

UiRectangle world_bounds(const ControlIndex &index, const UiControl &control) {
    auto bounds = control.bounds;
    auto parent = control.parent_name;
    while (!parent.empty()) {
        const auto found = index.find(canonical_name(parent));
        if (found == index.end())
            break;
        bounds.x += found->second->bounds.x;
        bounds.y += found->second->bounds.y;
        parent = found->second->parent_name;
    }
    return bounds;
}

UiRectangle pointer_bounds(
        const ControlIndex &index, const UiControl &control) {
    auto bounds = world_bounds(index, control);
    if (control.authored || !control.picture.empty() || bounds.width != 90 ||
            bounds.height != 46)
        return bounds;
    UiRectangle visual = bounds;
    if (control.kind == UiControlKind::knob)
        visual = { bounds.x - 20, bounds.y - 28, 40, 40 };
    else if (control.kind == UiControlKind::button ||
             control.kind == UiControlKind::switch_control)
        visual = { bounds.x - 22, bounds.y - 32, 84, 16 };
    else
        return bounds;
    const auto left = std::min(bounds.x, visual.x);
    const auto top = std::min(bounds.y, visual.y);
    const auto right =
            std::max(bounds.x + bounds.width, visual.x + visual.width);
    const auto bottom =
            std::max(bounds.y + bounds.height, visual.y + visual.height);
    return { left, top, right - left, bottom - top };
}

UiRectangle control_clip(const ControlIndex &index, const UiControl &control,
        const UiRectangle frame, const float scale) {
    auto result = frame;
    auto parent = control.parent_name;
    while (!parent.empty()) {
        const auto found = index.find(canonical_name(parent));
        if (found == index.end())
            break;
        auto bounds = world_bounds(index, *found->second);
        bounds.x = static_cast<int>(std::lround(bounds.x * scale));
        bounds.y = static_cast<int>(std::lround(bounds.y * scale));
        bounds.width = static_cast<int>(std::lround(bounds.width * scale));
        bounds.height = static_cast<int>(std::lround(bounds.height * scale));
        // panels never clip, other parents only if they have a size
        if (found->second->kind != UiControlKind::panel && bounds.width > 0 &&
                bounds.height > 0)
            result = intersect_rectangles(result, bounds);
        parent = found->second->parent_name;
    }
    return result;
}

bool control_hidden(const ControlIndex &index, const UiControl &control,
        const std::optional<std::size_t> performance_slot = {}) {
    if (performance_slot && !control.authored &&
            control.script_slot != *performance_slot)
        return true;
    bool hidden = control.hidden;
    auto parent = control.parent_name;
    while (!hidden && !parent.empty()) {
        const auto found = index.find(canonical_name(parent));
        if (found == index.end())
            break;
        hidden = found->second->hidden;
        parent = found->second->parent_name;
    }
    return hidden;
}

bool control_enabled(const ControlIndex &index, const UiControl &control) {
    bool enabled = control.enabled;
    auto parent = control.parent_name;
    while (enabled && !parent.empty()) {
        const auto found = index.find(canonical_name(parent));
        if (found == index.end())
            break;
        enabled = found->second->enabled;
        parent = found->second->parent_name;
    }
    return enabled;
}

void paint_fallback_control(
        Painter &painter, const UiControl &control, const float pixel_density) {
    const UiColor face{ 67, 70, 74, 255 };
    const UiColor edge{ 18, 19, 20, 255 };
    const UiColor accent{ 218, 153, 55, 255 };
    const UiColor track{ 30, 31, 33, 255 };
    const float value = normalized_value(control);
    const auto bounds = control.bounds;
    const auto pressed_offset =
            control.pressed &&
                            (control.kind == UiControlKind::button ||
                                    control.kind ==
                                            UiControlKind::switch_control) &&
                            !control.disable_text_shifting
                    ? 1
                    : 0;

    switch (control.kind) {
        case UiControlKind::knob: {
            // the grid position is the knob's center, NOT the top-left. 36px
            // across with text above and below
            const auto classic_scale =
                    bounds.width > 0 ? static_cast<float>(bounds.width) / 90.0F
                                     : 1.0F;
            const int radius = std::max(
                    static_cast<int>(std::lround(18.0F * classic_scale)), 3);
            const int cx = bounds.x;
            const int cy = bounds.y -
                           static_cast<int>(std::lround(8.0F * classic_scale));
            const int rim = std::max(
                    static_cast<int>(std::lround(2.0F * classic_scale)), 1);
            painter.circle(cx, cy, radius + rim, { 20, 20, 20, 255 });
            painter.circle(cx, cy, radius, { 205, 205, 201, 255 });
            painter.circle(
                    cx - static_cast<int>(std::lround(4.0F * classic_scale)),
                    cy - static_cast<int>(std::lround(4.0F * classic_scale)),
                    std::max(radius - static_cast<int>(std::lround(
                                              6.0F * classic_scale)),
                            1),
                    { 240, 240, 235, 255 });
            const float angle = -2.35F + value * 4.7F;
            painter.line(cx, cy,
                    cx + static_cast<int>(std::sin(angle) * (radius - 4)),
                    cy - static_cast<int>(std::cos(angle) * (radius - 4)),
                    { 52, 52, 52, 255 });

            std::string name = control.name;
            if (!name.empty() && name.front() == '$')
                name.erase(name.begin());
            std::replace(name.begin(), name.end(), '_', ' ');
            if (!name.empty()) {
                name.front() = static_cast<char>(
                        std::toupper(static_cast<unsigned char>(name.front())));
                if (std::ranges::any_of(name, [](unsigned char c) {
                        return std::isdigit(c) != 0;
                    }))
                    std::ranges::transform(
                            name, name.begin(), [](unsigned char c) {
                                return static_cast<char>(std::toupper(c));
                            });
                if (const auto raster =
                                rasterize_builtin_text(name, 1, classic_scale))
                    painter.text(cx + radius +
                                         static_cast<int>(std::lround(
                                                 6.0F * classic_scale)),
                            cy - static_cast<int>(
                                         std::lround(16.0F * classic_scale)),
                            *raster, { 245, 245, 245, 255 });
            }
            std::string display = control.text;
            if (!display.empty())
                if (const auto raster = rasterize_builtin_text(
                            display, 1, classic_scale))
                    painter.text(cx + radius +
                                         static_cast<int>(std::lround(
                                                 6.0F * classic_scale)),
                            cy + static_cast<int>(
                                         std::lround(4.0F * classic_scale)),
                            *raster, { 245, 245, 245, 255 });
            break;
        }
        case UiControlKind::slider: {
            painter.rectangle(bounds, track);
            painter.outline(bounds, edge);
            const int filled = static_cast<int>((bounds.height - 4) * value);
            painter.rectangle(
                    { bounds.x + 2, bounds.y + bounds.height - 2 - filled,
                            std::max(bounds.width - 4, 1), filled },
                    accent);
            break;
        }
        case UiControlKind::level_meter: {
            painter.rectangle(bounds, control.level_meter_off_color);
            painter.outline(bounds, edge);
            const auto amount = std::clamp(value, 0.0F, 1.1F);
            const auto fill_color =
                    amount >= 1.0F   ? control.level_meter_overload_color
                    : amount >= 0.9F ? control.level_meter_peak_color
                                     : control.level_meter_on_color;
            if (control.level_meter_vertical) {
                const auto filled = static_cast<int>(
                        std::lround(std::max(bounds.height - 2, 0) *
                                    std::min(amount, 1.0F)));
                const auto top = bounds.y + bounds.height - 1 - filled;
                if (control.level_meter_striped) {
                    for (auto y = top; y < bounds.y + bounds.height - 1; ++y)
                        if (((bounds.y + bounds.height + y) & 1) == 0)
                            painter.line(bounds.x + 1, y,
                                    bounds.x + bounds.width - 2, y, fill_color);
                } else {
                    painter.rectangle(
                            { bounds.x + 1, top, std::max(bounds.width - 2, 0),
                                    filled },
                            fill_color);
                }
            } else {
                const auto filled = static_cast<int>(
                        std::lround(std::max(bounds.width - 2, 0) *
                                    std::min(amount, 1.0F)));
                if (control.level_meter_striped) {
                    for (auto x = bounds.x + 1; x < bounds.x + 1 + filled; ++x)
                        if (((x - bounds.x) & 1) == 0)
                            painter.line(x, bounds.y + 1, x,
                                    bounds.y + bounds.height - 2, fill_color);
                } else {
                    painter.rectangle({ bounds.x + 1, bounds.y + 1, filled,
                                              std::max(bounds.height - 2, 0) },
                            fill_color);
                }
            }
            break;
        }
        case UiControlKind::button:
        case UiControlKind::switch_control: {
            const bool classic_grid =
                    bounds.width > 0 && bounds.height > 0 &&
                    std::abs(bounds.width * 46 - bounds.height * 90) <= 90;
            const auto classic_scale =
                    classic_grid ? static_cast<float>(bounds.width) / 90.0F
                                 : 1.0F;
            const UiRectangle button =
                    classic_grid
                            ? UiRectangle{ bounds.x -
                                                   static_cast<int>(std::lround(
                                                           22.0F *
                                                           classic_scale)),
                                  bounds.y - static_cast<int>(std::lround(
                                                     32.0F * classic_scale)),
                                  static_cast<int>(
                                          std::lround(84.0F * classic_scale)),
                                  static_cast<int>(
                                          std::lround(16.0F * classic_scale)) }
                            : bounds;
            const auto top = value > 0.5F ? UiColor{ 247, 247, 247, 255 }
                                          : UiColor{ 120, 120, 120, 255 };
            const auto bottom = value > 0.5F ? UiColor{ 190, 190, 190, 255 }
                                             : UiColor{ 73, 73, 73, 255 };
            for (int y = 0; y < button.height; ++y) {
                const auto amount =
                        static_cast<float>(y) /
                        static_cast<float>(std::max(button.height - 1, 1));
                const auto mix = [amount](std::uint8_t a, std::uint8_t b) {
                    return static_cast<std::uint8_t>(std::lround(
                            a + amount * (static_cast<float>(b) - a)));
                };
                painter.rectangle({ button.x, button.y + y, button.width, 1 },
                        { mix(top.red, bottom.red),
                                mix(top.green, bottom.green),
                                mix(top.blue, bottom.blue), 255 });
            }
            painter.outline(button, { 35, 35, 35, 255 });
            if (pressed_offset)
                painter.line(button.x + 1, button.y + 1,
                        button.x + button.width - 2, button.y + 1,
                        { 55, 55, 55, 255 });
            if (!control.text.empty())
                if (const auto raster = rasterize_builtin_text(
                            control.text, 1, classic_scale))
                    painter.text(button.x +
                                         static_cast<int>(std::lround(
                                                 5.0F * classic_scale)) +
                                         pressed_offset,
                            button.y + (button.height - raster->height) / 2 +
                                    pressed_offset,
                            *raster,
                            value > 0.5F ? UiColor{ 25, 25, 25, 255 }
                                         : UiColor{ 245, 245, 245, 255 });
            break;
        }
        case UiControlKind::table:
        case UiControlKind::waveform:
            painter.rectangle(bounds, track);
            painter.outline(bounds, edge);
            painter.line(bounds.x + 2, bounds.y + bounds.height / 2,
                    bounds.x + bounds.width - 3, bounds.y + bounds.height / 2,
                    accent);
            break;
        case UiControlKind::xy: {
            painter.rectangle(bounds, track);
            painter.outline(bounds, edge);
            if (const auto *values =
                            std::get_if<std::vector<double>>(&control.value)) {
                for (std::size_t index = 0; index + 1 < values->size();
                        index += 2) {
                    const auto px =
                            bounds.x +
                            static_cast<int>(std::lround(
                                    std::clamp((*values)[index], 0.0, 1.0) *
                                    std::max(bounds.width - 1, 0)));
                    // clang-format off
                    const auto py = bounds.y +
                            static_cast<int>(std::lround((1.0 -
                            std::clamp((*values)[index + 1], 0.0, 1.0)) *
                            std::max(bounds.height - 1, 0)));
                    // clang-format on
                    painter.circle(px, py,
                            index / 2 == static_cast<std::size_t>(std::max(
                                                 control.xy_active_index, 0))
                                    ? 4
                                    : 3,
                            accent);
                }
            }
            break;
        }
        case UiControlKind::label:
            if (!control.authored && bounds.width > 0 && bounds.height > 0) {
                // a tall label doesn't mean big text, it's just layout space
                const auto caption =
                        control.text.empty() ? control.label : control.text;
                if (!caption.empty())
                    // clang-format off
                    if (const auto raster = rasterize_builtin_text(caption,
                            control.font_type, pixel_density))
                        // clang-format on
                        painter.text(
                                bounds.x + std::max((bounds.width -
                                                            raster->width) /
                                                            2,
                                                   0),
                                bounds.y + std::max((bounds.height -
                                                            raster->height) /
                                                            2,
                                                   0),
                                *raster,
                                declared_font_color(control,
                                        integer_value(control.value) != 0)
                                        .value_or(control.text_color));
            }
            break;
        case UiControlKind::text_edit:
        case UiControlKind::panel:
            // labels have no face, panels are transparent without a picture
            break;
        default:
            painter.rectangle(bounds, face);
            painter.outline(bounds, edge);
            break;
    }
    std::string caption = control.text.empty() ? control.label : control.text;
    if (control.kind == UiControlKind::knob ||
            control.kind == UiControlKind::button ||
            control.kind == UiControlKind::switch_control ||
            control.kind == UiControlKind::label)
        caption.clear();
    if (caption.empty() && control.kind == UiControlKind::text_edit) {
        if (const auto *value = std::get_if<std::string>(&control.value))
            caption = *value;
    }
    painter.text(bounds.x + 4 + pressed_offset, bounds.y + 4 + pressed_offset,
            caption, control.text_color);
}

} // namespace

std::int32_t UiMenuPopupLayout::item_at(
        const std::int32_t x, const std::int32_t y) const {
    if (!viewport.contains(x, y))
        return -1;
    const auto index = static_cast<std::int32_t>(
            std::floor((y - viewport.y + scroll_offset) / row_height));
    return index >= 0 && index < static_cast<std::int32_t>(entries.size())
                   ? index
                   : -1;
}

UiMenuPopupLayout ui_menu_popup_layout(
        const UiDocument &document, const UiControl &menu) {
    UiMenuPopupLayout result;
    int width = 160;
    for (std::size_t index = 0; index < menu.menu_entries.size(); ++index) {
        if (index < menu.menu_entry_visible.size() &&
                !menu.menu_entry_visible[index])
            continue;
        result.entries.push_back(index);
        if (const auto text = ui_rasterize_builtin_text(
                    menu.menu_entries[index].second, -100))
            width = std::max(width, text->width + 20);
    }
    // no max width, just fit it in the window margins
    const int margin_x = std::min(16, std::max((document.width - 1) / 2, 0));
    const int margin_y = std::min(16, std::max((document.height - 1) / 2, 0));
    width = std::min(width, std::max(document.width - margin_x * 2, 1));
    const auto content_height = static_cast<double>(result.entries.size()) * 24;
    const auto height = static_cast<int>(std::min(content_height + 8,
            static_cast<double>(std::max(document.height - margin_y * 2, 1))));
    const auto anchor = document.bounds_of(menu);
    result.bounds = {
        std::clamp(anchor.x - 3, margin_x,
                std::max(document.width - margin_x - width, margin_x)),
        std::clamp(anchor.y + anchor.height - 5, margin_y,
                std::max(document.height - margin_y - height, margin_y)),
        width, height
    };
    result.viewport = { result.bounds.x, result.bounds.y + 4, width,
        std::max(height - 8, 0) };
    result.maximum_scroll =
            std::max(content_height - result.viewport.height, 0.0);
    result.scroll_offset =
            std::clamp(menu.menu_scroll_offset, 0.0, result.maximum_scroll);
    return result;
}

std::shared_ptr<const UiRasterizedText> ui_rasterize_builtin_text(
        const std::string_view text, const std::int32_t font_type,
        const float pixel_density) {
    static std::mutex mutex;
    static std::unordered_map<std::string,
            std::shared_ptr<const UiRasterizedText>>
            cache;
    const auto key = std::to_string(font_type) + ":" +
                     std::to_string(static_cast<int>(
                             std::lround(pixel_density * 1024))) +
                     ":" + std::string{ text };
    std::lock_guard lock(mutex);
    if (const auto found = cache.find(key); found != cache.end())
        return found->second;
    const auto raster = rasterize_builtin_text(text, font_type, pixel_density);
    auto result = raster ? std::make_shared<const UiRasterizedText>(*raster)
                         : std::shared_ptr<const UiRasterizedText>{};
    cache.emplace(key, result);
    return result;
}

std::shared_ptr<const UiRasterizedText> ui_rasterize_font_text(
        const std::string_view text, const std::span<const std::byte> font,
        const std::uint32_t pixel_size) {
    if (font.empty() || pixel_size == 0)
        return {};
    static FT_Library library{};
    static std::mutex mutex;
    static std::unordered_map<std::string,
            std::shared_ptr<const UiRasterizedText>>
            cache;
    std::lock_guard lock(mutex);
    const auto key =
            std::to_string(reinterpret_cast<std::uintptr_t>(font.data())) +
            ":" + std::to_string(font.size()) + ":" +
            std::to_string(pixel_size) + ":" + std::string{ text };
    if (const auto found = cache.find(key); found != cache.end())
        return found->second;
    if (!library && FT_Init_FreeType(&library) != 0)
        return {};
    FT_Face face{};
    if (FT_New_Memory_Face(library,
                reinterpret_cast<const FT_Byte *>(font.data()),
                static_cast<FT_Long>(font.size()), 0, &face) != 0)
        return {};
    const auto cleanup = [&] { FT_Done_Face(face); };
    if (FT_Select_Charmap(face, FT_ENCODING_UNICODE) != 0 ||
            FT_Set_Pixel_Sizes(face, 0, pixel_size) != 0) {
        cleanup();
        return {};
    }
    const auto codepoints = decode_utf8(text);
    const auto rounded = [](const FT_Pos value) {
        return static_cast<int>((value + 32) / 64);
    };
    const auto line_height = std::max(rounded(face->size->metrics.height), 1);
    const auto ascender = rounded(face->size->metrics.ascender);
    int pen{};
    FT_UInt previous{};
    for (const auto codepoint : codepoints) {
        const auto glyph = FT_Get_Char_Index(face, codepoint);
        if (previous && FT_HAS_KERNING(face)) {
            FT_Vector delta{};
            if (FT_Get_Kerning(
                        face, previous, glyph, FT_KERNING_DEFAULT, &delta) == 0)
                pen += rounded(delta.x);
        }
        if (FT_Load_Glyph(face, glyph, FT_LOAD_RENDER) == 0)
            pen += rounded(face->glyph->advance.x);
        previous = glyph;
    }
    UiRasterizedText result;
    result.width = std::max(pen, 0);
    result.height = line_height;
    result.alpha.assign(
            static_cast<std::size_t>(result.width) * result.height, 0);
    pen = 0;
    previous = 0;
    for (const auto codepoint : codepoints) {
        const auto glyph = FT_Get_Char_Index(face, codepoint);
        if (previous && FT_HAS_KERNING(face)) {
            FT_Vector delta{};
            if (FT_Get_Kerning(
                        face, previous, glyph, FT_KERNING_DEFAULT, &delta) == 0)
                pen += rounded(delta.x);
        }
        if (FT_Load_Glyph(face, glyph, FT_LOAD_RENDER) == 0) {
            const auto &bitmap = face->glyph->bitmap;
            const auto origin_x = pen + face->glyph->bitmap_left;
            const auto origin_y = ascender - face->glyph->bitmap_top;
            for (unsigned y = 0; y < bitmap.rows; ++y)
                for (unsigned x = 0; x < bitmap.width; ++x) {
                    const auto target_x = origin_x + static_cast<int>(x);
                    const auto target_y = origin_y + static_cast<int>(y);
                    if (target_x >= 0 && target_x < result.width &&
                            target_y >= 0 && target_y < result.height)
                        result.alpha[static_cast<std::size_t>(target_y) *
                                             result.width +
                                     target_x] =
                                bitmap.buffer[static_cast<std::ptrdiff_t>(y) *
                                                      bitmap.pitch +
                                              x];
                }
            pen += rounded(face->glyph->advance.x);
        }
        previous = glyph;
    }
    cleanup();
    auto cached = std::make_shared<const UiRasterizedText>(std::move(result));
    cache.emplace(key, cached);
    return cached;
}

bool UiRectangle::contains(const std::int32_t px, const std::int32_t py) const {
    return px >= x && py >= y && px < x + width && py < y + height;
}

UiControl *UiDocument::find(const std::size_t slot, const std::int64_t id) {
    const auto found =
            std::ranges::find_if(controls, [&](const UiControl &item) {
                return item.script_slot == slot && item.ksp_id == id;
            });
    return found == controls.end() ? nullptr : &*found;
}

const UiControl *UiDocument::find(
        const std::size_t slot, const std::int64_t id) const {
    return const_cast<UiDocument *>(this)->find(slot, id);
}

int classic_control_order(const UiControlKind kind) {
    // by type, NOT declaration order, and keep that order inside each z layer
    switch (kind) {
        case UiControlKind::panel:
            return 0;
        case UiControlKind::file_selector:
            return 1;
        case UiControlKind::waveform:
            return 2;
        case UiControlKind::wavetable:
            return 3;
        case UiControlKind::level_meter:
            return 4;
        case UiControlKind::label:
            return 5;
        case UiControlKind::knob:
            return 6;
        case UiControlKind::slider:
            return 7;
        case UiControlKind::switch_control:
            return 8;
        case UiControlKind::button:
            return 9;
        case UiControlKind::value_edit:
            return 10;
        case UiControlKind::menu:
            return 11;
        case UiControlKind::table:
            return 12;
        case UiControlKind::xy:
            return 13;
        case UiControlKind::mouse_area:
            return 14;
        case UiControlKind::text_edit:
            return 15;
        case UiControlKind::unknown:
            return 16;
    }
    return 16;
}

UiControl *UiDocument::hit_test(const std::int32_t x, const std::int32_t y) {
    const auto index = control_index(*this);
    UiControl *found{};
    for (const auto *ordered : classic_paint_order()) {
        auto &item = *const_cast<UiControl *>(ordered);
        const bool accepts_pointer =
                item.kind == UiControlKind::button ||
                item.kind == UiControlKind::switch_control ||
                item.kind == UiControlKind::knob ||
                item.kind == UiControlKind::slider ||
                item.kind == UiControlKind::value_edit ||
                item.kind == UiControlKind::menu ||
                item.kind == UiControlKind::table ||
                item.kind == UiControlKind::xy ||
                item.kind == UiControlKind::waveform ||
                item.kind == UiControlKind::wavetable ||
                item.kind == UiControlKind::file_selector ||
                item.kind == UiControlKind::text_edit ||
                item.kind == UiControlKind::mouse_area;
        if (control_hidden(index, item, performance_view_script_slot) ||
                !control_enabled(index, item) || !accepts_pointer ||
                !pointer_bounds(index, item).contains(x, y))
            continue;
        // same order as painting
        found = &item;
    }
    return found;
}

const UiControl *UiDocument::hit_test(
        const std::int32_t x, const std::int32_t y) const {
    return const_cast<UiDocument *>(this)->hit_test(x, y);
}

bool UiDocument::visible(const UiControl &control) const {
    return !control_hidden(
            control_index(*this), control, performance_view_script_slot);
}

bool UiDocument::enabled(const UiControl &control) const {
    return control_enabled(control_index(*this), control);
}

UiRectangle UiDocument::bounds_of(const UiControl &control) const {
    return world_bounds(control_index(*this), control);
}

std::vector<UiResolvedControl> UiDocument::resolve_controls() const {
    const auto index = control_index(*this);
    std::vector<UiResolvedControl> result;
    result.reserve(controls.size());
    for (const auto &control : controls)
        result.push_back({ &control, world_bounds(index, control),
                !control_hidden(index, control, performance_view_script_slot),
                control_enabled(index, control) });
    return result;
}

std::vector<const UiControl *> UiDocument::classic_paint_order() const {
    // sort per panel, a global sort lets controls poke through modal panels
    const auto index = control_index(*this);
    const auto root = controls.size();
    std::vector<std::vector<std::size_t>> children(root + 1);
    for (std::size_t i = 0; i < root; ++i) {
        auto parent = root;
        const auto found = index.find(canonical_name(controls[i].parent_name));
        if (found != index.end() &&
                found->second->kind == UiControlKind::panel &&
                found->second->script_slot == controls[i].script_slot)
            parent = static_cast<std::size_t>(found->second - controls.data());
        children[parent].push_back(i);
    }
    for (auto &siblings : children)
        std::ranges::stable_sort(siblings, [&](auto a, auto b) {
            if (controls[a].z_layer != controls[b].z_layer)
                return controls[a].z_layer < controls[b].z_layer;
            return classic_control_order(controls[a].kind) <
                   classic_control_order(controls[b].kind);
        });
    std::vector<const UiControl *> result;
    result.reserve(root);
    const auto append = [&](auto &&self, std::size_t parent) -> void {
        for (const auto child : children[parent]) {
            result.push_back(&controls[child]);
            self(self, child);
        }
    };
    append(append, root);
    return result;
}

bool UiImage::valid() const {
    return width != 0 && height != 0 &&
           rgba.size() == static_cast<std::size_t>(width) * height * 4;
}

std::optional<UiRectangle> ui_xy_cursor_bounds(const UiControl &control,
        const std::size_t cursor, const UiAssetProvider *assets,
        const float scale) {
    const auto *values = std::get_if<std::vector<double>>(&control.value);
    const auto first = cursor * 2;
    if (!assets || !values || first >= values->size() ||
            first >= control.xy_cursor_pictures.size() ||
            control.xy_cursor_pictures[first].empty())
        return std::nullopt;
    const auto image = assets->image(control.xy_cursor_pictures[first], scale);
    if (!image || !image->valid())
        return std::nullopt;
    const auto frames = std::max(image->animation_count, 1U);
    const auto density = std::max(image->pixel_density, 1.0F);
    // clang-format off
    const auto width = std::max(1, static_cast<int>(std::lround((image->width /
            (image->horizontal_animation ? frames : 1U)) / density * scale)));
    // clang-format on
    // clang-format off
    const auto height = std::max(1,
            static_cast<int>(std::lround((image->height /
            (image->horizontal_animation ? 1U : frames)) / density * scale)));
    // clang-format on
    return UiRectangle{ control.bounds.x +
                                static_cast<int>(std::lround(
                                        std::clamp((*values)[first], 0.0, 1.0) *
                                        (control.bounds.width - width))),
        control.bounds.y +
                static_cast<int>(std::lround(
                        (1.0 - std::clamp(first + 1 < values->size()
                                                  ? (*values)[first + 1]
                                                  : 0.0,
                                       0.0, 1.0)) *
                        (control.bounds.height - height))),
        width, height };
}

UiControlKind ui_control_kind(const std::string_view type) {
    if (type == "ui_button")
        return UiControlKind::button;
    if (type == "ui_switch")
        return UiControlKind::switch_control;
    if (type == "ui_knob")
        return UiControlKind::knob;
    if (type == "ui_slider")
        return UiControlKind::slider;
    if (type == "ui_value_edit")
        return UiControlKind::value_edit;
    if (type == "ui_label")
        return UiControlKind::label;
    if (type == "ui_menu")
        return UiControlKind::menu;
    if (type == "ui_table")
        return UiControlKind::table;
    if (type == "ui_xy")
        return UiControlKind::xy;
    if (type == "ui_waveform")
        return UiControlKind::waveform;
    if (type == "ui_level_meter")
        return UiControlKind::level_meter;
    if (type == "ui_file_selector")
        return UiControlKind::file_selector;
    if (type == "ui_text_edit")
        return UiControlKind::text_edit;
    if (type == "ui_panel")
        return UiControlKind::panel;
    if (type == "ui_wavetable")
        return UiControlKind::wavetable;
    if (type == "ui_mouse_area")
        return UiControlKind::mouse_area;
    return UiControlKind::unknown;
}

std::int32_t ui_control_type_id(const UiControlKind kind) {
    // kontakt 8's control type numbers
    switch (kind) {
        case UiControlKind::button:
            return 1;
        case UiControlKind::knob:
            return 2;
        case UiControlKind::menu:
            return 3;
        case UiControlKind::value_edit:
            return 4;
        case UiControlKind::label:
            return 5;
        case UiControlKind::table:
            return 6;
        case UiControlKind::waveform:
            return 7;
        case UiControlKind::wavetable:
            return 8;
        case UiControlKind::slider:
            return 9;
        case UiControlKind::text_edit:
            return 10;
        case UiControlKind::file_selector:
            return 11;
        case UiControlKind::switch_control:
            return 12;
        case UiControlKind::xy:
            return 13;
        case UiControlKind::level_meter:
            return 14;
        case UiControlKind::mouse_area:
            return 15;
        case UiControlKind::panel:
            return 16;
        case UiControlKind::unknown:
            return 0;
    }
    return 0;
}

UiFrame UiRenderer::render(const UiDocument &document, float scale,
        std::vector<NativeUiHitRegion> *native_hits) const {
    scale = std::clamp(scale, 0.25F, 4.0F);
    constexpr int classic_tab_height = 20;
    const bool native_ui = !document.native_ui_component_roots.empty();
    const auto content_height =
            document.performance_view_content_height.value_or(document.height);
    const auto presentation_height =
            native_ui                                ? document.height
            : document.performance_view_tabs.empty() ? document.height
            : document.performance_view_content_height
                    ? document.height
                    : document.height + classic_tab_height;
    UiFrame frame;
    frame.width = static_cast<std::uint32_t>(
            std::max(1L, std::lround(document.width * scale)));
    frame.height = static_cast<std::uint32_t>(
            std::max(1L, std::lround(presentation_height * scale)));
    frame.stride = frame.width * 4;
    frame.rgba.resize(static_cast<std::size_t>(frame.stride) * frame.height);
    Painter painter(frame);
    painter.rectangle({ 0, 0, static_cast<int>(frame.width),
                              static_cast<int>(frame.height) },
            document.background);

    if (!document.loading_instrument.empty()) {
        const int width = static_cast<int>(frame.width);
        const int middle = static_cast<int>(frame.height) / 2;
        const int bar_width =
                std::min(static_cast<int>(400 * scale), width * 3 / 4);
        const int left = (width - bar_width) / 2;
        const auto text = [&](const std::string &value, int y, UiColor color) {
            if (const auto raster = ui_rasterize_font_text(value,
                        std::as_bytes(
                                std::span(embedded_fonts::open_sans_semibold)),
                        std::max(1L, std::lround(15 * scale)))) {
                painter.set_clip(
                        { left, 0, bar_width, static_cast<int>(frame.height) });
                painter.text(std::max(left, (width - raster->width) / 2), y,
                        *raster, color);
            }
        };
        text("Loading instrument", middle - static_cast<int>(52 * scale),
                { 245, 240, 246, 255 });
        const int height = std::max(3, static_cast<int>(5 * scale));
        painter.rectangle({ left, middle + height, bar_width, height },
                { 59, 48, 66, 255 });
        const int segment = std::max(1, bar_width / 4);
        const float phase = std::clamp(document.loading_animation, 0.F, 1.F);
        const float triangle = phase < .5F ? phase * 2 : (1 - phase) * 2;
        painter.rectangle(
                { left + static_cast<int>((bar_width - segment) * triangle),
                        middle + height, segment, height },
                { 201, 165, 213, 255 });
        text(document.loading_instrument, middle + static_cast<int>(30 * scale),
                { 190, 174, 199, 255 });
        return frame;
    }

    if (!native_ui && asset_provider && !document.background_picture.empty())
        if (const auto image = asset_provider->image(
                    document.background_picture, scale)) {
            const auto density = std::max(image->pixel_density, 1.0F);
            const auto logical_height =
                    static_cast<int>(std::lround(image->height / density));
            auto offset = std::clamp(document.background_offset_y, 0,
                    std::max(logical_height - 1, 0));
            // the view starts under kontakt's 0x44 pixel header, but only skip
            // it if a whole page still fits below, the atlas can have padding
            // so the stride isn't the crop
            constexpr int legacy_instrument_header_height = 0x44;
            if (logical_height - offset >=
                    content_height + legacy_instrument_header_height)
                offset += legacy_instrument_header_height;
            else if (image->animation_count > 1 &&
                     !image->horizontal_animation) {
                const auto frame_height =
                        logical_height /
                        static_cast<int>(image->animation_count);
                if (frame_height >= content_height)
                    offset += frame_height - content_height;
            }
            offset = std::min(offset, logical_height - 1);
            const auto source_offset =
                    static_cast<int>(std::lround(offset * density));
            const auto source_height = static_cast<int>(std::lround(
                    std::min(content_height, logical_height - offset) *
                    density));
            painter.image_region(*image,
                    { 0, 0, static_cast<int>(frame.width),
                            static_cast<int>(
                                    std::lround(content_height * scale)) },
                    { 0, source_offset, static_cast<int>(image->width),
                            source_height });
        }

    render_native_ui_components(
            document, asset_provider, frame, scale, native_hits);

    // it replaces the KSP view, it doesn't go on top
    if (native_ui)
        return frame;

    const auto ordered = document.classic_paint_order();
    const auto index = control_index(document);
    const UiRectangle frame_clip{ 0, 0, static_cast<int>(frame.width),
        static_cast<int>(frame.height) };
    for (const auto *source_pointer : ordered) {
        const auto &source = *source_pointer;
        if (control_hidden(
                    index, source, document.performance_view_script_slot))
            continue;
        UiControl control = source;
        // the label's value (often -3) isn't a font selector
        if (control.kind == UiControlKind::label)
            control.value = std::int64_t{ 0 };
        if (const auto color = declared_font_color(
                    control, integer_value(control.value) != 0))
            control.text_color = *color;
        const auto logical_bounds = world_bounds(index, source);
        control.bounds = logical_bounds;
        control.bounds.x =
                static_cast<int>(std::lround(control.bounds.x * scale));
        control.bounds.y =
                static_cast<int>(std::lround(control.bounds.y * scale));
        control.bounds.width =
                static_cast<int>(std::lround(control.bounds.width * scale));
        control.bounds.height =
                static_cast<int>(std::lround(control.bounds.height * scale));
        const auto ancestor_clip =
                control_clip(index, source, frame_clip, scale);
        auto clip = ancestor_clip;
        if (control.bounds.width > 0 && control.bounds.height > 0)
            clip = intersect_rectangles(clip, control.bounds);
        painter.set_clip(clip);
        bool painted = false;
        const bool has_assigned_picture = !control.picture.empty();
        if (asset_provider && has_assigned_picture &&
                (control.hide_parts & 1) == 0) {
            if (const auto image =
                            asset_provider->image(control.picture, scale)) {
                const auto frame_width =
                        image->horizontal_animation
                                ? std::max(
                                          image->width /
                                                  std::max(
                                                          image->animation_count,
                                                          1U),
                                          1U)
                                : image->width;
                const auto frame_height =
                        image->horizontal_animation
                                ? image->height
                                : std::max(
                                          image->height /
                                                  std::max(
                                                          image->animation_count,
                                                          1U),
                                          1U);
                if (control.bounds.width <= 0)
                    control.bounds.width = static_cast<int>(std::lround(
                            frame_width / std::max(image->pixel_density, 1.0F) *
                            scale));
                if (control.bounds.height <= 0)
                    control.bounds.height = static_cast<int>(std::lround(
                            frame_height /
                            std::max(image->pixel_density, 1.0F) * scale));
                auto state = control.picture_state;
                const bool automatic_picture_state =
                        !control.picture_state_explicit ||
                        control.picture_state == 0;
                if (automatic_picture_state &&
                        (control.kind == UiControlKind::knob ||
                                control.kind == UiControlKind::slider ||
                                control.kind == UiControlKind::level_meter))
                    state = static_cast<std::int32_t>(std::lround(
                            normalized_value(control) *
                            static_cast<float>(
                                    std::max(image->animation_count, 1U) -
                                    1U)));
                if (automatic_picture_state &&
                        (control.kind == UiControlKind::button ||
                                control.kind ==
                                        UiControlKind::switch_control) &&
                        image->animation_count >= 2)
                    state = integer_value(control.value) != 0 ? 1 : 0;
                if (automatic_picture_state &&
                        (control.kind == UiControlKind::button ||
                                control.kind ==
                                        UiControlKind::switch_control) &&
                        image->animation_count >= 4) {
                    // idle, clicked, hotspot, two frames each with six, click
                    // wins
                    if (image->animation_count >= 6)
                        state += control.pressed ? 2 : control.hovered ? 4 : 0;
                    else if (control.hovered)
                        state += 2;
                }
                auto image_bounds = control.bounds;
                // one pixel in, the hit rectangle doesn't change
                if (!native_ui &&
                        (control.kind == UiControlKind::button ||
                                control.kind == UiControlKind::switch_control ||
                                control.kind == UiControlKind::menu)) {
                    const auto inset = static_cast<int>(std::lround(scale));
                    image_bounds.x += inset;
                    image_bounds.y += inset;
                    image_bounds.width =
                            std::max(image_bounds.width - 2 * inset, 0);
                    image_bounds.height =
                            std::max(image_bounds.height - 2 * inset, 0);
                }
                const auto image_scale =
                        scale / std::max(image->pixel_density, 1.0F);
                const auto intrinsic_width = static_cast<int>(
                        std::lround(frame_width * image_scale));
                const auto intrinsic_height = static_cast<int>(
                        std::lround(frame_height * image_scale));
                const auto padding_limit =
                        static_cast<int>(std::lround(4.0F * scale));
                // the picture keeps its size per axis even if the control is
                // wider or shorter, and keeps its origin
                const bool intrinsic_slider =
                        !native_ui && control.kind == UiControlKind::slider;
                const bool anchored_picture =
                        !native_ui &&
                        (control.kind == UiControlKind::button ||
                                control.kind == UiControlKind::switch_control ||
                                control.kind == UiControlKind::label ||
                                control.kind == UiControlKind::menu);
                if (intrinsic_slider || !control.width_explicit ||
                        !image->horizontal_resizable) {
                    // clang-format off
                    if (!intrinsic_slider && !anchored_picture &&
                            intrinsic_width <= image_bounds.width &&
                            image_bounds.width - intrinsic_width <=
                            padding_limit)
                        // clang-format on
                        image_bounds.x +=
                                (image_bounds.width - intrinsic_width) / 2;
                    image_bounds.width = intrinsic_width;
                }
                if (intrinsic_slider || !control.height_explicit ||
                        !image->vertical_resizable) {
                    // clang-format off
                    if (!intrinsic_slider && !anchored_picture &&
                            intrinsic_height <= image_bounds.height &&
                            image_bounds.height - intrinsic_height <=
                            padding_limit)
                        // clang-format on
                        image_bounds.y +=
                                (image_bounds.height - intrinsic_height) / 2;
                    image_bounds.height = intrinsic_height;
                }
                // don't crop it to the control again
                painter.set_clip(ancestor_clip);
                painter.image(*image, image_bounds, state,
                        scale / std::max(image->pixel_density, 1.0F));
                if (control.hovered && control.picture == "button_big" &&
                        (control.kind == UiControlKind::button ||
                                control.kind == UiControlKind::switch_control))
                    painter.rectangle(image_bounds, { 190, 190, 190, 40 });
                if (!control.text.empty() || !control.menu_entries.empty())
                    control.text_color = authored_font_color(
                            control, integer_value(control.value) != 0, *image);
                painted = true;
            }
        }
        if (control.kind == UiControlKind::waveform) {
            paint_authored_waveform(painter, control);
            painted = true;
        }
        if (control.kind == UiControlKind::table) {
            paint_authored_table(painter, control);
            painted = true;
        }
        if (control.kind == UiControlKind::xy) {
            // HIDE_PART_BG doesn't hide the cursors
            paint_xy_control(painter, control, asset_provider, scale);
            painted = true;
        }
        // menus show their entry no matter what the title/value flags say
        const bool visible_caption = control.kind == UiControlKind::menu ||
                                     (control.hide_parts & (2 | 4)) == 0;
        if (control.authored || control.kind == UiControlKind::label ||
                control.kind == UiControlKind::text_edit) {
            if (visible_caption && control.bounds.width > 0 &&
                    control.bounds.height > 0) {
                // text can overhang the control, only the parents clip it
                painter.set_clip(ancestor_clip);
                auto text_control = control;
                if (text_control.pressed &&
                        (text_control.kind == UiControlKind::button ||
                                text_control.kind ==
                                        UiControlKind::switch_control) &&
                        !text_control.disable_text_shifting) {
                    const auto pressed_offset =
                            static_cast<int>(std::max(std::lround(scale), 1L));
                    text_control.bounds.x += pressed_offset;
                    text_control.bounds.y += pressed_offset;
                }
                paint_authored_text(painter, text_control, asset_provider,
                        scale, native_ui);
            }
        } else if (painted && visible_caption &&
                   (!control.text.empty() ||
                           (control.kind == UiControlKind::menu &&
                                   !control.menu_entries.empty()))) {
            // the picture only replaces the frame, the text still gets drawn
            painter.set_clip(
                    intersect_rectangles(ancestor_clip, control.bounds));
            paint_authored_text(
                    painter, control, asset_provider, scale, native_ui);
        } else if (!painted && control.kind != UiControlKind::mouse_area &&
                   !has_assigned_picture) {
            // mouse areas draw nothing, and a picture that isn't loaded yet
            // stays transparent instead of the default face. stock controls
            // stick out of their rectangle on purpose
            painter.set_clip(ancestor_clip);
            paint_fallback_control(painter, control, scale);
        }
    }
    painter.set_clip(frame_clip);
    for (const auto *source_pointer : ordered) {
        if (!source_pointer->menu_open ||
                source_pointer->menu_entries.empty() ||
                control_hidden(index, *source_pointer,
                        document.performance_view_script_slot))
            continue;
        const auto popup = ui_menu_popup_layout(document, *source_pointer);
        const auto scaled = [scale](UiRectangle rect) {
            return UiRectangle{ static_cast<int>(std::lround(rect.x * scale)),
                static_cast<int>(std::lround(rect.y * scale)),
                static_cast<int>(std::lround(rect.width * scale)),
                static_cast<int>(std::lround(rect.height * scale)) };
        };
        const auto bounds = scaled(popup.bounds);
        // Lumen Level2, three shadows under surface03
        for (const auto shadow : { std::array<float, 4>{ 3, 5, -1, .20F },
                     { 6, 10, 0, .14F }, { 1, 18, 0, .12F } }) {
            const auto blur = std::max(shadow[1] * scale / 2, 1.0F);
            const auto spread = shadow[2] * scale;
            const auto offset = shadow[0] * scale;
            const auto extent = static_cast<int>(std::ceil(blur));
            for (int y = bounds.y - extent;
                    y < bounds.y + bounds.height + extent + offset; ++y)
                for (int x = bounds.x - extent;
                        x < bounds.x + bounds.width + extent; ++x) {
                    if (bounds.contains(x, y))
                        continue;
                    const auto dx = std::max({ float(bounds.x - x) - spread,
                            float(x - bounds.x - bounds.width + 1) - spread,
                            0.0F });
                    const auto dy = std::max({ bounds.y + offset - y - spread,
                            y - (bounds.y + bounds.height + offset - 1) -
                                    spread,
                            0.0F });
                    const auto alpha = std::clamp(
                            1.0F - std::hypot(dx, dy) / blur, 0.0F, 1.0F);
                    if (alpha > 0)
                        painter.rectangle({ x, y, 1, 1 },
                                { 0, 0, 0,
                                        static_cast<std::uint8_t>(
                                                255 * shadow[3] * alpha) });
                }
        }
        painter.rounded_rectangle(bounds, { 60, 60, 60, 255 }, 2 * scale);
        const auto viewport = scaled(popup.viewport);
        painter.set_clip(intersect_rectangles(frame_clip, viewport));
        for (std::size_t ordinal = 0; ordinal < popup.entries.size();
                ++ordinal) {
            const int y = static_cast<int>(std::lround(
                    (popup.viewport.y + ordinal * 24.0 - popup.scroll_offset) *
                    scale));
            const UiRectangle row{ bounds.x, y, bounds.width,
                static_cast<int>(std::lround(24 * scale)) };
            if (row.y + row.height <= viewport.y ||
                    row.y >= viewport.y + viewport.height)
                continue;
            if (source_pointer->menu_hover_index == static_cast<int>(ordinal))
                painter.rectangle(row, { 91, 91, 91, 255 });
            if (const auto raster = ui_rasterize_builtin_text(
                        source_pointer->menu_entries[popup.entries[ordinal]]
                                .second,
                        -100, scale))
                painter.text(row.x + static_cast<int>(std::lround(10 * scale)),
                        row.y + (row.height - raster->height) / 2, *raster,
                        { 255, 255, 255, 245 });
        }
        if (popup.maximum_scroll > 0 &&
                source_pointer->menu_scroll_opacity > 0) {
            const auto content_height = popup.entries.size() * 24.0;
            const auto thumb_height = std::max(
                    4.0, popup.viewport.height * popup.viewport.height /
                                 content_height);
            const auto thumb_y =
                    popup.viewport.y +
                    popup.scroll_offset *
                            (popup.viewport.height - thumb_height) /
                            popup.maximum_scroll;
            painter.rounded_rectangle(
                    { bounds.x + bounds.width - static_cast<int>(8 * scale),
                            static_cast<int>(std::lround(thumb_y * scale)),
                            std::max(static_cast<int>(4 * scale), 1),
                            static_cast<int>(
                                    std::lround(thumb_height * scale)) },
                    { 74, 74, 74,
                            static_cast<std::uint8_t>(
                                    255 *
                                    std::clamp(
                                            source_pointer->menu_scroll_opacity,
                                            0.0F, 1.0F)) },
                    2 * scale);
        }
        painter.set_clip(frame_clip);
    }
    if (!document.performance_view_tabs.empty()) {
        // the tab strip under the script's controls
        constexpr int tab_width = 125;
        const auto y = static_cast<int>(std::lround(content_height * scale));
        painter.set_clip(frame_clip);
        painter.rectangle({ 0, y, static_cast<int>(frame.width),
                                  static_cast<int>(frame.height) - y },
                { 105, 105, 105, 255 });
        int x = 2;
        for (const auto &[slot, title] : document.performance_view_tabs) {
            const auto width = static_cast<int>(std::lround(tab_width * scale));
            const auto selected =
                    document.performance_view_script_slot &&
                    *document.performance_view_script_slot == slot;
            const UiRectangle tab{ x, y + 1, width - 2,
                static_cast<int>(frame.height) - y - 1 };
            painter.rectangle(tab, selected ? UiColor{ 7, 7, 7, 255 }
                                            : UiColor{ 104, 104, 104, 255 });
            painter.outline(tab, selected ? UiColor{ 25, 25, 25, 255 }
                                          : UiColor{ 42, 42, 42, 255 });
            if (const auto raster = rasterize_builtin_text(title, 1, scale))
                painter.text(tab.x + 7,
                        tab.y + std::max((tab.height - raster->height) / 2, 0),
                        *raster, { 238, 238, 238, 255 });
            x += width;
        }
    }
    // only for [0.01, 0.99), at the top since standalone has no header
    if (document.loading_progress && *document.loading_progress >= 0.01F &&
            *document.loading_progress < 0.99F) {
        painter.set_clip(frame_clip);
        const auto progress =
                std::clamp(*document.loading_progress, 0.0F, 1.0F);
        const int height =
                std::max(2, static_cast<int>(std::lround(3.0F * scale)));
        painter.rectangle({ 0, 0, static_cast<int>(frame.width), height },
                { 31, 31, 31, 220 });
        painter.rectangle(
                { 0, 0, static_cast<int>(std::floor(frame.width * progress)),
                        height },
                { 205, 205, 205, 255 });
    }
    return frame;
}

} // namespace ekphore
