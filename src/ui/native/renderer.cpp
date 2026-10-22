// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "renderer.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>

namespace ekphore {
namespace {

thread_local std::int64_t profile_ring_us{};
thread_local std::size_t profile_ring_count{};

std::shared_ptr<const std::vector<std::byte>> platform_ui_font() {
#if defined(__APPLE__)
    static const auto font = [] {
        auto bytes = std::make_shared<std::vector<std::byte>>();
        std::ifstream input("/System/Library/Fonts/SFNS.ttf",
                std::ios::binary | std::ios::ate);
        if (!input)
            return decltype(bytes){};
        const auto size = input.tellg();
        if (size <= 0)
            return decltype(bytes){};
        bytes->resize(static_cast<std::size_t>(size));
        input.seekg(0);
        input.read(reinterpret_cast<char *>(bytes->data()), size);
        if (!input)
            return decltype(bytes){};
        return bytes;
    }();
    return font;
#else
    return {};
#endif
}

struct Size {
    double width{};
    double height{};
};

struct AxisConstraints {
    double minimum{};
    double maximum{};
    int priority{};
};

struct Insets {
    double left{};
    double right{};
    double top{};
    double bottom{};
};

struct Alignment {
    std::string_view vertical;
    std::string_view horizontal;
};

Alignment parse_alignment(const std::string_view value) {
    if (value.empty())
        return {};
    const auto separator = value.find(' ');
    if (separator == std::string_view::npos) {
        if (value == "top" || value == "bottom")
            return { value, {} };
        if (value == "left" || value == "right")
            return { {}, value };
        if (value == "center")
            return { value, value };
        return {};
    }
    const auto first = value.substr(0, separator);
    const auto second = value.substr(separator + 1);
    // compound alignment stores vertical before horizontal, eg top center
    return { first, second };
}

const NativeUiProperty *property(const std::vector<NativeUiProperty> &values,
        const std::string_view name) {
    const auto found = std::ranges::find(values, name, &NativeUiProperty::name);
    return found == values.end() ? nullptr : &*found;
}

const NativeUiModifier *modifier(
        const NativeUiComponentNode &node, const std::string_view name) {
    const auto found =
            std::ranges::find(node.modifiers, name, &NativeUiModifier::name);
    return found == node.modifiers.end() ? nullptr : &*found;
}

const NativeUiModifier *outer_modifier(
        const NativeUiComponentNode &node, const std::string_view name) {
    const auto found = std::find_if(node.modifiers.rbegin(),
            node.modifiers.rend(), [name](const NativeUiModifier &value) {
                return value.name == name;
            });
    return found == node.modifiers.rend() ? nullptr : &*found;
}

std::optional<double> number(const NativeUiProperty *value) {
    if (!value)
        return std::nullopt;
    if (const auto *result = std::get_if<double>(&value->value))
        return *result;
    if (const auto *result = std::get_if<bool>(&value->value))
        return *result ? 1.0 : 0.0;
    return std::nullopt;
}

std::optional<double> number(const std::vector<NativeUiProperty> &values,
        const std::string_view name) {
    return number(property(values, name));
}

std::optional<std::string_view>
text(const std::vector<NativeUiProperty> &values, const std::string_view name) {
    const auto *value = property(values, name);
    if (!value)
        return std::nullopt;
    if (const auto *result = std::get_if<std::string>(&value->value))
        return *result;
    return std::nullopt;
}

bool flag(const std::vector<NativeUiProperty> &values,
        const std::string_view name, const bool fallback = false) {
    const auto *value = property(values, name);
    if (!value)
        return fallback;
    if (const auto *result = std::get_if<bool>(&value->value))
        return *result;
    if (const auto *result = std::get_if<double>(&value->value))
        return *result != 0.0;
    return fallback;
}

std::optional<UiColor> color(const std::vector<NativeUiProperty> &values,
        const std::string_view name) {
    const auto encoded = text(values, name);
    if (!encoded || !encoded->starts_with("argb:"))
        return std::nullopt;
    std::uint32_t argb{};
    const auto digits = encoded->substr(5);
    const auto [end, error] =
            std::from_chars(digits.data(), digits.data() + digits.size(), argb);
    if (error != std::errc{} || end != digits.data() + digits.size())
        return std::nullopt;
    return UiColor{ static_cast<std::uint8_t>((argb >> 16U) & 0xffU),
        static_cast<std::uint8_t>((argb >> 8U) & 0xffU),
        static_cast<std::uint8_t>(argb & 0xffU),
        static_cast<std::uint8_t>((argb >> 24U) & 0xffU) };
}

std::optional<double> argument(const NativeUiModifier *value,
        const std::string_view named,
        const std::string_view positional = "#1") {
    if (!value)
        return std::nullopt;
    if (const auto result = number(value->arguments, named))
        return result;
    return number(value->arguments, positional);
}

Size frame_size(const NativeUiComponentNode &node) {
    // the last frame is the outermost one, that's what the parent sees
    const auto *value = outer_modifier(node, "frame");
    if (!value)
        return {};
    return { number(value->arguments, "width").value_or(0.0),
        number(value->arguments, "height").value_or(0.0) };
}

std::optional<double> frame_limit(
        const NativeUiComponentNode &node, const std::string_view name) {
    const auto *value = outer_modifier(node, "frame");
    return value ? number(value->arguments, name) : std::nullopt;
}

Insets padding(const NativeUiComponentNode &node) {
    const auto *value = modifier(node, "padding");
    if (!value)
        return {};
    const auto all = number(value->arguments, "#1").value_or(0.0);
    const auto horizontal =
            number(value->arguments, "horizontal").value_or(all);
    const auto vertical = number(value->arguments, "vertical").value_or(all);
    return { number(value->arguments, "left").value_or(horizontal),
        number(value->arguments, "right").value_or(horizontal),
        number(value->arguments, "top").value_or(vertical),
        number(value->arguments, "bottom").value_or(vertical) };
}

Insets padding(const NativeUiModifier &value) {
    const auto all = number(value.arguments, "#1").value_or(0.0);
    const auto horizontal = number(value.arguments, "horizontal").value_or(all);
    const auto vertical = number(value.arguments, "vertical").value_or(all);
    return { number(value.arguments, "left").value_or(horizontal),
        number(value.arguments, "right").value_or(horizontal),
        number(value.arguments, "top").value_or(vertical),
        number(value.arguments, "bottom").value_or(vertical) };
}

Size apply_layout_modifiers(const NativeUiComponentNode &node, Size size,
        const std::size_t count = std::numeric_limits<std::size_t>::max()) {
    // inside out, padding after a frame makes it bigger, padding inside
    // doesn't
    for (std::size_t index = 0; index < node.modifiers.size() && index < count;
            ++index) {
        const auto &value = node.modifiers[index];
        if (value.name == "padding") {
            const auto inset = padding(value);
            size.width += inset.left + inset.right;
            size.height += inset.top + inset.bottom;
        } else if (value.name == "frame") {
            if (const auto width = number(value.arguments, "width"))
                size.width = *width;
            if (const auto height = number(value.arguments, "height"))
                size.height = *height;
            if (const auto minimum = number(value.arguments, "min_width"))
                size.width = std::max(size.width, *minimum);
            if (const auto minimum = number(value.arguments, "min_height"))
                size.height = std::max(size.height, *minimum);
            if (const auto maximum = number(value.arguments, "max_width"))
                size.width = std::min(size.width, *maximum);
            if (const auto maximum = number(value.arguments, "max_height"))
                size.height = std::min(size.height, *maximum);
        }
    }
    return size;
}

bool hidden(const NativeUiComponentNode &node) {
    const auto *value = modifier(node, "hidden");
    return argument(value, "hidden").value_or(0.0) != 0.0;
}

double opacity(const NativeUiComponentNode &node) {
    return std::clamp(
            argument(modifier(node, "opacity"), "opacity").value_or(1.0), 0.0,
            1.0);
}

std::string_view short_kind(const NativeUiComponentNode &node) {
    const auto separator = node.kind.rfind('.');
    return separator == std::string::npos
                   ? std::string_view(node.kind)
                   : std::string_view(node.kind).substr(separator + 1);
}

std::string_view layout_kind(const NativeUiComponentNode &node) {
    const auto kind = short_kind(node);
    if (kind != "List")
        return kind;
    return text(node.properties, "orientation").value_or("vertical") ==
                           "horizontal"
                   ? std::string_view{ "HStack" }
                   : std::string_view{ "VStack" };
}

bool flexible_axis(const NativeUiComponentNode &node, const bool horizontal) {
    for (auto it = node.modifiers.rbegin(); it != node.modifiers.rend(); ++it) {
        if (it->name == "frame" &&
                number(it->arguments, horizontal ? "width" : "height"))
            return false;
        if (it->name == "frame")
            if (const auto maximum = number(
                        it->arguments, horizontal ? "max_width" : "max_height");
                    maximum && *maximum >= 0.0)
                return std::isinf(*maximum);
        // an aligned image keeps its size, it must not make the ZStack
        // unbounded
        if (it->name == "align" &&
                !(short_kind(node) == "Image" &&
                        !flag(node.properties, "resizable"))) {
            const auto alignment =
                    parse_alignment(text(it->arguments, "#1").value_or(""));
            if (horizontal ? !alignment.horizontal.empty()
                           : !alignment.vertical.empty())
                return true;
        }
    }
    const auto fixed = frame_size(node);
    if (horizontal ? fixed.width > 0.0 : fixed.height > 0.0)
        return false;
    // Ring has a minimum but no maximum, it picks a square out of whatever it
    // gets
    if (short_kind(node) == "Ring")
        return true;
    if (short_kind(node) == "Rectangle")
        return true;
    if (short_kind(node) == "Spacer")
        return true;
    const auto parent_layout = layout_kind(node);
    return std::ranges::any_of(
            node.children, [horizontal, parent_layout](const auto &child) {
                if (child.role.starts_with("modifier:"))
                    return false;
                // a Spacer only stretches along its own stack, letting it leak
                // to the cross axis breaks the HStack's centering
                if (short_kind(child) == "Spacer" && child.modifiers.empty())
                    return horizontal ? parent_layout == "HStack"
                                      : parent_layout == "VStack";
                return flexible_axis(child, horizontal);
            });
}

bool descendant_flexible_axis(
        const NativeUiComponentNode &node, const bool horizontal) {
    return std::ranges::any_of(node.children, [horizontal](const auto &child) {
        return !child.role.starts_with("modifier:") &&
               flexible_axis(child, horizontal);
    });
}

bool descendant_alignment_axis(
        const NativeUiComponentNode &node, const bool horizontal) {
    return std::ranges::any_of(node.children, [horizontal](const auto &child) {
        if (child.role.starts_with("modifier:"))
            return false;
        if (const auto *value = modifier(child, "align")) {
            const auto alignment =
                    parse_alignment(text(value->arguments, "#1").value_or(""));
            if (horizontal ? !alignment.horizontal.empty()
                           : !alignment.vertical.empty())
                return true;
        }
        return descendant_alignment_axis(child, horizontal);
    });
}

bool aligned_overlay_requires_proposal(
        const NativeUiComponentNode &node, const bool horizontal) {
    if (layout_kind(node) != "ZStack")
        return false;
    return std::ranges::any_of(node.children, [horizontal](const auto &child) {
        if (child.role.starts_with("modifier:"))
            return false;
        if (short_kind(child) == "Image" &&
                !flag(child.properties, "resizable"))
            return false;
        const auto *value = modifier(child, "align");
        if (!value)
            return false;
        const auto alignment =
                parse_alignment(text(value->arguments, "#1").value_or(""));
        // only edge alignment needs the whole proposal, a centered one (eg a
        // slider handle) doesn't
        return horizontal ? alignment.horizontal == "left" ||
                                    alignment.horizontal == "right"
                          : alignment.vertical == "top" ||
                                    alignment.vertical == "bottom";
    });
}

bool descendant_aligned_overlay_requires_proposal(
        const NativeUiComponentNode &node, const bool horizontal) {
    if (aligned_overlay_requires_proposal(node, horizontal))
        return true;
    if (layout_kind(node) != "ZStack")
        return false;
    return std::ranges::any_of(node.children, [horizontal](const auto &child) {
        return !child.role.starts_with("modifier:") &&
               descendant_aligned_overlay_requires_proposal(child, horizontal);
    });
}

void append_layout_children(const NativeUiComponentNode &node,
        std::vector<const NativeUiComponentNode *> &result) {
    for (const auto &child : node.children) {
        if (child.role.starts_with("modifier:"))
            continue;
        // hidden still takes space, it just doesn't paint or take input.
        // ForEach has no node of its own, its children go straight in the
        // stack
        if (short_kind(child) == "ForEach")
            append_layout_children(child, result);
        else
            result.push_back(&child);
    }
}

class NativeCanvas {
  public:
    explicit NativeCanvas(UiFrame &frame) : target_frame(frame) {}

    void image(const UiImage &source, UiRectangle destination,
            const double opacity, const double offset_x = 0.0,
            const double offset_y = 0.0) {
        image_region(source, destination,
                { 0, 0, static_cast<int>(source.width),
                        static_cast<int>(source.height) },
                opacity, offset_x, offset_y);
    }

    void rotated_image(const UiImage &source, const UiRectangle destination,
            const UiRectangle pivot_bounds, const double degrees,
            const double opacity) {
        if (!source.valid() || destination.width <= 0 ||
                destination.height <= 0 || opacity <= 0.0)
            return;
        const auto radians = degrees * 0.017453292519943295;
        const auto cosine = std::cos(radians);
        const auto sine = std::sin(radians);
        const auto pivot_x = pivot_bounds.x + pivot_bounds.width * 0.5;
        const auto pivot_y = pivot_bounds.y + pivot_bounds.height * 0.5;
        const std::array corners{ std::pair{ static_cast<double>(destination.x),
                                          static_cast<double>(destination.y) },
            std::pair{ static_cast<double>(destination.x + destination.width),
                    static_cast<double>(destination.y) },
            std::pair{ static_cast<double>(destination.x),
                    static_cast<double>(destination.y + destination.height) },
            std::pair{ static_cast<double>(destination.x + destination.width),
                    static_cast<double>(destination.y + destination.height) } };
        auto min_x = std::numeric_limits<double>::max();
        auto min_y = std::numeric_limits<double>::max();
        auto max_x = std::numeric_limits<double>::lowest();
        auto max_y = std::numeric_limits<double>::lowest();
        for (const auto [x, y] : corners) {
            const auto dx = x - pivot_x;
            const auto dy = y - pivot_y;
            const auto rx = pivot_x + dx * cosine - dy * sine;
            const auto ry = pivot_y + dx * sine + dy * cosine;
            min_x = std::min(min_x, rx);
            min_y = std::min(min_y, ry);
            max_x = std::max(max_x, rx);
            max_y = std::max(max_y, ry);
        }
        const auto x0 = std::max(static_cast<int>(std::floor(min_x)), 0);
        const auto y0 = std::max(static_cast<int>(std::floor(min_y)), 0);
        const auto x1 = std::min(static_cast<int>(std::ceil(max_x)),
                static_cast<int>(target_frame.width));
        const auto y1 = std::min(static_cast<int>(std::ceil(max_y)),
                static_cast<int>(target_frame.height));
        for (auto y = y0; y < y1; ++y)
            for (auto x = x0; x < x1; ++x) {
                const auto dx = x + 0.5 - pivot_x;
                const auto dy = y + 0.5 - pivot_y;
                const auto ux = pivot_x + dx * cosine + dy * sine;
                const auto uy = pivot_y - dx * sine + dy * cosine;
                if (ux < destination.x ||
                        ux >= destination.x + destination.width ||
                        uy < destination.y ||
                        uy >= destination.y + destination.height)
                    continue;
                const auto sx = std::clamp(
                        (ux - destination.x) * source.width / destination.width,
                        0.0, static_cast<double>(source.width) - 1.0);
                // clang-format off
                const auto sy = std::clamp((uy - destination.y) *
                        source.height / destination.height, 0.0,
                        static_cast<double>(source.height) - 1.0);
                // clang-format on
                const auto source_x = static_cast<std::uint32_t>(sx);
                const auto source_y = static_cast<std::uint32_t>(sy);
                const auto source_offset =
                        (static_cast<std::size_t>(source_y) * source.width +
                                source_x) *
                        4;
                pixel(x, y,
                        { source.rgba[source_offset],
                                source.rgba[source_offset + 1],
                                source.rgba[source_offset + 2],
                                source.rgba[source_offset + 3] },
                        opacity);
            }
    }

    void image_region(const UiImage &source, UiRectangle destination,
            UiRectangle region, const double opacity,
            const double offset_x = 0.0, const double offset_y = 0.0) {
        if (!source.valid() || destination.width <= 0 ||
                destination.height <= 0 || region.width <= 0 ||
                region.height <= 0 || opacity <= 0.0)
            return;
        region.x = std::clamp(region.x, 0, static_cast<int>(source.width) - 1);
        region.y = std::clamp(region.y, 0, static_cast<int>(source.height) - 1);
        region.width = std::min(
                region.width, static_cast<int>(source.width) - region.x);
        region.height = std::min(
                region.height, static_cast<int>(source.height) - region.y);
        const auto x0 = std::max(
                static_cast<int>(std::floor(destination.x + offset_x)), 0);
        const auto y0 = std::max(
                static_cast<int>(std::floor(destination.y + offset_y)), 0);
        const auto x1 =
                std::min(static_cast<int>(std::ceil(
                                 destination.x + offset_x + destination.width)),
                        static_cast<int>(target_frame.width));
        const auto y1 =
                std::min(static_cast<int>(std::ceil(destination.y + offset_y +
                                                    destination.height)),
                        static_cast<int>(target_frame.height));
        const bool exact = destination.width == region.width &&
                           destination.height == region.height;
        // don't change the asset's density, KSP controls still use it
        if (!exact) {
            static std::mutex cache_mutex;
            static std::unordered_map<std::string,
                    std::shared_ptr<const UiImage>>
                    cache;
            const auto key = std::to_string(reinterpret_cast<std::uintptr_t>(
                                     source.rgba.data())) +
                             ':' + std::to_string(region.x) + ':' +
                             std::to_string(region.y) + ':' +
                             std::to_string(region.width) + ':' +
                             std::to_string(region.height) + ':' +
                             std::to_string(destination.width) + ':' +
                             std::to_string(destination.height);
            std::shared_ptr<const UiImage> scaled;
            {
                std::lock_guard lock(cache_mutex);
                if (const auto found = cache.find(key); found != cache.end())
                    scaled = found->second;
                else {
                    UiImage image;
                    image.width = static_cast<std::uint32_t>(destination.width);
                    image.height =
                            static_cast<std::uint32_t>(destination.height);
                    image.pixel_density = source.pixel_density;
                    image.opaque = true;
                    image.rgba.resize(static_cast<std::size_t>(image.width) *
                                      image.height * 4);
                    struct SampleX {
                        std::size_t left, right;
                        double fraction;
                    };
                    std::vector<SampleX> samples_x;
                    if (source.opaque) {
                        samples_x.reserve(destination.width);
                        for (int x = 0; x < destination.width; ++x) {
                            // clang-format off
                            const auto sx = std::clamp(region.x + (x + 0.5) *
                                    region.width / destination.width - 0.5,
                                    static_cast<double>(region.x),
                                    static_cast<double>(region.x +
                                    region.width - 1));
                            // clang-format on
                            const auto left =
                                    static_cast<std::size_t>(std::floor(sx));
                            samples_x.push_back({ left * 4,
                                    std::min(left + 1,
                                            static_cast<std::size_t>(
                                                    region.x + region.width -
                                                    1)) *
                                            4,
                                    sx - left });
                        }
                    }
                    for (int y = 0; y < destination.height; ++y) {
                        // clang-format off
                        const auto sy = std::clamp(region.y + (y + 0.5) *
                                region.height / destination.height - 0.5,
                                static_cast<double>(region.y),
                                static_cast<double>(region.y + region.height -
                                1));
                        // clang-format on
                        const auto sy0 =
                                static_cast<std::uint32_t>(std::floor(sy));
                        const auto sy1 = std::min(
                                sy0 + 1, static_cast<std::uint32_t>(
                                                 region.y + region.height - 1));
                        const auto fy = sy - sy0;
                        if (source.opaque) {
                            const auto *top = source.rgba.data() +
                                              static_cast<std::size_t>(sy0) *
                                                      source.width * 4;
                            const auto *bottom = source.rgba.data() +
                                                 static_cast<std::size_t>(sy1) *
                                                         source.width * 4;
                            auto *target = image.rgba.data() +
                                           static_cast<std::size_t>(y) *
                                                   image.width * 4;
                            for (const auto &sample : samples_x) {
                                const auto a =
                                        (1.0 - sample.fraction) * (1.0 - fy);
                                const auto b = sample.fraction * (1.0 - fy);
                                const auto c = (1.0 - sample.fraction) * fy;
                                const auto d = sample.fraction * fy;
                                for (int channel = 0; channel < 3; ++channel)
                                    // clang-format off
                                    target[channel] =
                                            static_cast<std::uint8_t>(
                                            std::clamp(
                                            top[sample.left + channel] * a +
                                            top[sample.right + channel] * b +
                                            bottom[sample.left + channel] * c +
                                            bottom[sample.right + channel] * d,
                                            0.0, 255.0));
                                // clang-format on
                                target[3] = 255;
                                target += 4;
                            }
                            continue;
                        }
                        for (int x = 0; x < destination.width; ++x) {
                            // clang-format off
                            const auto sx = std::clamp(region.x + (x + 0.5) *
                                    region.width / destination.width - 0.5,
                                    static_cast<double>(region.x),
                                    static_cast<double>(region.x +
                                    region.width - 1));
                            // clang-format on
                            const auto sx0 =
                                    static_cast<std::uint32_t>(std::floor(sx));
                            const auto sx1 = std::min(sx0 + 1,
                                    static_cast<std::uint32_t>(
                                            region.x + region.width - 1));
                            const auto fx = sx - sx0;
                            const auto target =
                                    (static_cast<std::size_t>(y) * image.width +
                                            x) *
                                    4;
                            double sampled_alpha{};
                            double sampled_rgb[3]{};
                            const std::array samples{
                                std::tuple{ sx0, sy0, (1.0 - fx) * (1.0 - fy) },
                                std::tuple{ sx1, sy0, fx * (1.0 - fy) },
                                std::tuple{ sx0, sy1, (1.0 - fx) * fy },
                                std::tuple{ sx1, sy1, fx * fy }
                            };
                            for (const auto [px, py, weight] : samples) {
                                // clang-format off
                                const auto offset =
                                        (static_cast<std::size_t>(py) *
                                        source.width + px) * 4;
                                // clang-format on
                                const auto alpha =
                                        source.rgba[offset + 3] / 255.0;
                                sampled_alpha += alpha * weight;
                                for (std::size_t channel = 0; channel < 3;
                                        ++channel)
                                    sampled_rgb[channel] +=
                                            source.rgba[offset + channel] *
                                            alpha * weight;
                            }
                            for (std::size_t channel = 0; channel < 3;
                                    ++channel) {
                                image.rgba[target + channel] =
                                        static_cast<std::uint8_t>(std::clamp(
                                                sampled_alpha > 0.0
                                                        ? sampled_rgb[channel] /
                                                                  sampled_alpha
                                                        : 0.0,
                                                0.0, 255.0));
                            }
                            image.rgba[target + 3] =
                                    static_cast<std::uint8_t>(std::clamp(
                                            sampled_alpha * 255.0, 0.0, 255.0));
                            image.opaque = image.opaque &&
                                           image.rgba[target + 3] == 255;
                        }
                    }
                    scaled = std::make_shared<const UiImage>(std::move(image));
                    cache.emplace(key, scaled);
                }
            }
            image_region(*scaled, destination,
                    { 0, 0, destination.width, destination.height }, opacity,
                    offset_x, offset_y);
            return;
        }
        if (offset_x == 0.0 && offset_y == 0.0) {
            const auto opaque = opacity >= 1.0 && source.opaque;
            if (opaque) {
                for (auto y = y0; y < y1; ++y) {
                    // clang-format off
                    const auto source_offset =
                            (static_cast<std::size_t>(region.y + y -
                            destination.y) * source.width + region.x + x0 -
                            destination.x) * 4;
                    // clang-format on
                    const auto target_offset =
                            static_cast<std::size_t>(y) * target_frame.stride +
                            static_cast<std::size_t>(x0) * 4;
                    std::copy_n(source.rgba.begin() + source_offset,
                            static_cast<std::size_t>(x1 - x0) * 4,
                            target_frame.rgba.begin() + target_offset);
                }
            } else {
                for (auto y = y0; y < y1; ++y)
                    for (auto x = x0; x < x1; ++x) {
                        // clang-format off
                        const auto source_offset =
                                (static_cast<std::size_t>(region.y + y -
                                destination.y) * source.width + region.x + x -
                                destination.x) * 4;
                        // clang-format on
                        pixel(x, y,
                                { source.rgba[source_offset],
                                        source.rgba[source_offset + 1],
                                        source.rgba[source_offset + 2],
                                        source.rgba[source_offset + 3] },
                                opacity);
                    }
            }
            return;
        }
        for (auto y = y0; y < y1; ++y) {
            for (auto x = x0; x < x1; ++x) {
                const auto sample_x =
                        static_cast<double>(region.x + x - destination.x) -
                        offset_x;
                const auto sample_y =
                        static_cast<double>(region.y + y - destination.y) -
                        offset_y;
                const auto clamped_x = std::clamp(sample_x,
                        static_cast<double>(region.x),
                        static_cast<double>(region.x + region.width - 1));
                const auto clamped_y = std::clamp(sample_y,
                        static_cast<double>(region.y),
                        static_cast<double>(region.y + region.height - 1));
                const auto sx0 =
                        static_cast<std::uint32_t>(std::floor(clamped_x));
                const auto sy0 =
                        static_cast<std::uint32_t>(std::floor(clamped_y));
                const auto sx1 =
                        std::min(sx0 + 1, static_cast<std::uint32_t>(
                                                  region.x + region.width - 1));
                const auto sy1 = std::min(
                        sy0 + 1, static_cast<std::uint32_t>(
                                         region.y + region.height - 1));
                const auto fx = clamped_x - sx0;
                const auto fy = clamped_y - sy0;
                double sampled_alpha{};
                double sampled_rgb[3]{};
                const std::array samples{ std::tuple{ sx0, sy0,
                                                  (1.0 - fx) * (1.0 - fy) },
                    std::tuple{ sx1, sy0, fx * (1.0 - fy) },
                    std::tuple{ sx0, sy1, (1.0 - fx) * fy },
                    std::tuple{ sx1, sy1, fx * fy } };
                for (const auto [sx, sy, w] : samples) {
                    const auto source_offset =
                            (static_cast<std::size_t>(sy) * source.width + sx) *
                            4;
                    const auto a = source.rgba[source_offset + 3] / 255.0;
                    sampled_alpha += a * w;
                    for (std::size_t channel = 0; channel < 3; ++channel)
                        sampled_rgb[channel] +=
                                source.rgba[source_offset + channel] * a * w;
                }
                const auto target_offset =
                        static_cast<std::size_t>(y) * target_frame.stride +
                        static_cast<std::size_t>(x) * 4;
                const auto coverage_x = std::max(0.0,
                        std::min(x + 1.0,
                                destination.x + offset_x + destination.width) -
                                std::max<double>(x, destination.x + offset_x));
                const auto coverage_y = std::max(0.0,
                        std::min(y + 1.0,
                                destination.y + offset_y + destination.height) -
                                std::max<double>(y, destination.y + offset_y));
                const auto alpha =
                        sampled_alpha * opacity * coverage_x * coverage_y;
                for (std::size_t channel = 0; channel < 3; ++channel) {
                    const auto sampled =
                            sampled_alpha > 0.0
                                    ? sampled_rgb[channel] / sampled_alpha
                                    : 0.0;
                    // clang-format off
                    target_frame.rgba[target_offset + channel] =
                            static_cast<std::uint8_t>(std::clamp(sampled *
                            alpha + target_frame.rgba[target_offset + channel] *
                            (1.0 - alpha), 0.0, 255.0));
                    // clang-format on
                }
                // clang-format off
                target_frame.rgba[target_offset + 3] =
                        static_cast<std::uint8_t>(std::clamp(255.0 * alpha +
                        target_frame.rgba[target_offset + 3] * (1.0 - alpha),
                        0.0, 255.0));
                // clang-format on
            }
        }
    }

    void rectangle(const UiRectangle bounds, const UiColor color,
            const double opacity) {
        const auto x0 = std::max(bounds.x, 0);
        const auto y0 = std::max(bounds.y, 0);
        const auto x1 = std::min(
                bounds.x + bounds.width, static_cast<int>(target_frame.width));
        const auto y1 = std::min(bounds.y + bounds.height,
                static_cast<int>(target_frame.height));
        if (color.alpha == 255 && opacity >= 1.0) {
            for (auto y = y0; y < y1; ++y)
                for (auto x = x0; x < x1; ++x) {
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
        for (auto y = y0; y < y1; ++y)
            for (auto x = x0; x < x1; ++x)
                pixel(x, y, color, opacity);
    }

    void rounded_rectangle(const UiRectangle bounds, const double radius,
            const UiColor color, const double opacity) {
        const auto r = std::clamp(
                radius, 0.0, std::min(bounds.width, bounds.height) * 0.5);
        if (r <= 0.0) {
            rectangle(bounds, color, opacity);
            return;
        }
        const auto x0 = std::max(bounds.x, 0);
        const auto y0 = std::max(bounds.y, 0);
        const auto x1 = std::min(
                bounds.x + bounds.width, static_cast<int>(target_frame.width));
        const auto y1 = std::min(bounds.y + bounds.height,
                static_cast<int>(target_frame.height));
        const auto left = bounds.x + r;
        const auto right = bounds.x + bounds.width - r;
        const auto top = bounds.y + r;
        const auto bottom = bounds.y + bounds.height - r;
        for (auto y = y0; y < y1; ++y)
            for (auto x = x0; x < x1; ++x) {
                const auto px = x + 0.5;
                const auto py = y + 0.5;
                const auto cx = std::clamp(px, left, right);
                const auto cy = std::clamp(py, top, bottom);
                const auto distance = std::hypot(px - cx, py - cy);
                const auto coverage = std::clamp(r + 0.5 - distance, 0.0, 1.0);
                if (coverage > 0.0)
                    pixel(x, y, color, opacity * coverage);
            }
    }

    void line(int x0, int y0, const int x1, const int y1,
            const double thickness, const UiColor color, const double opacity) {
        const auto dx = static_cast<double>(x1 - x0);
        const auto dy = static_cast<double>(y1 - y0);
        const auto length = std::hypot(dx, dy);
        if (length <= 0.0)
            return;
        const auto radius = std::max(thickness * 0.5, 0.5);
        const auto min_x =
                static_cast<int>(std::floor(std::min(x0, x1) - radius - 1.0));
        const auto max_x =
                static_cast<int>(std::ceil(std::max(x0, x1) + radius + 1.0));
        const auto min_y =
                static_cast<int>(std::floor(std::min(y0, y1) - radius - 1.0));
        const auto max_y =
                static_cast<int>(std::ceil(std::max(y0, y1) + radius + 1.0));
        for (auto y = min_y; y <= max_y; ++y)
            for (auto x = min_x; x <= max_x; ++x) {
                const auto px = x + 0.5 - x0;
                const auto py = y + 0.5 - y0;
                const auto projection = std::clamp(
                        (px * dx + py * dy) / (length * length), 0.0, 1.0);
                const auto distance =
                        std::hypot(px - projection * dx, py - projection * dy);
                const auto coverage =
                        std::clamp(radius + 0.5 - distance, 0.0, 1.0);
                if (coverage > 0.0)
                    pixel(x, y, color, opacity * coverage);
            }
    }

    void ring(const UiRectangle bounds, const double thickness,
            const double angle, const double rotation, const UiColor color,
            const double opacity) {
        const auto profile_start = std::chrono::steady_clock::now();
        ++profile_ring_count;
        const auto center_x = bounds.x + bounds.width * 0.5;
        const auto center_y = bounds.y + bounds.height * 0.5;
        const auto radius = std::max(
                std::min(bounds.width, bounds.height) * 0.5 - 0.5, 0.0);
        const auto inner = std::max(radius - std::max(thickness, 0.0), 0.0);
        const auto sweep = std::clamp(std::abs(angle), 0.0, 360.0);
        const auto outer_limit = (radius + 0.5) * (radius + 0.5);
        const auto inner_edge = std::max(inner - 0.5, 0.0);
        const auto inner_limit = inner_edge * inner_edge;
        constexpr auto radians_to_degrees = 57.29577951308232;
        constexpr auto degrees_to_radians = 0.017453292519943295;
        const auto start_radians =
                (rotation - (angle < 0.0 ? sweep : 0.0)) * degrees_to_radians;
        const auto end_radians = start_radians + sweep * degrees_to_radians;
        const auto start_x = std::cos(start_radians);
        const auto start_y = std::sin(start_radians);
        const auto end_x = std::cos(end_radians);
        const auto end_y = std::sin(end_radians);
        const auto y_end = std::min(bounds.y + bounds.height,
                static_cast<int>(target_frame.height));
        const auto x_end = std::min(
                bounds.x + bounds.width, static_cast<int>(target_frame.width));
        for (auto y = std::max(bounds.y, 0); y < y_end; ++y) {
            const auto dy = y + 0.5 - center_y;
            const auto outer_row = outer_limit - dy * dy;
            if (outer_row <= 0.0)
                continue;
            const auto outer_reach = std::sqrt(outer_row);
            const auto first = std::max(bounds.x,
                    std::max(static_cast<int>(
                                     std::floor(center_x - outer_reach - 0.5)),
                            0));
            const auto last = std::min(x_end,
                    static_cast<int>(std::ceil(center_x + outer_reach - 0.5)) +
                            1);
            const auto paint_span = [&](const int begin, const int end) {
                for (auto x = begin; x < end; ++x) {
                    const auto dx = x + 0.5 - center_x;
                    const auto distance_squared = dx * dx + dy * dy;
                    if (distance_squared >= outer_limit ||
                            (inner > 0.0 && distance_squared <= inner_limit))
                        continue;
                    const auto distance = std::sqrt(distance_squared);
                    const auto outer_coverage =
                            std::clamp(radius + 0.5 - distance, 0.0, 1.0);
                    const auto inner_coverage =
                            inner <= 0.0 ? 1.0
                                         : std::clamp(distance - inner + 0.5,
                                                   0.0, 1.0);
                    const auto coverage =
                            std::min(outer_coverage, inner_coverage);
                    if (coverage <= 0.0)
                        continue;
                    if (sweep > 0.0 && sweep < 360.0) {
                        const auto after_start = start_x * dy - start_y * dx;
                        const auto before_end = dx * end_y - dy * end_x;
                        const bool inside =
                                sweep <= 180.0 ? after_start >= -1.0e-10 &&
                                                         before_end >= -1.0e-10
                                               : after_start >= -1.0e-10 ||
                                                         before_end >= -1.0e-10;
                        if (!inside)
                            continue;
                    } else if (sweep == 0.0) {
                        auto degrees = std::atan2(dy, dx) * radians_to_degrees -
                                       rotation;
                        if (angle < 0.0)
                            degrees = -degrees;
                        degrees = std::fmod(degrees, 360.0);
                        if (degrees < 0.0)
                            degrees += 360.0;
                        if (degrees > 0.0)
                            continue;
                    }
                    pixel(x, y, color, opacity * coverage);
                }
            };
            if (inner > 0.0 && inner_limit > dy * dy) {
                const auto inner_reach = std::sqrt(inner_limit - dy * dy);
                const auto left_end = std::min(
                        last, static_cast<int>(
                                      std::ceil(center_x - inner_reach - 0.5)) +
                                      1);
                const auto right_begin = std::max(first,
                        static_cast<int>(
                                std::floor(center_x + inner_reach - 0.5)) -
                                1);
                paint_span(first, left_end);
                paint_span(std::max(left_end, right_begin), last);
            } else {
                paint_span(first, last);
            }
        }
        profile_ring_us +=
                std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - profile_start)
                        .count();
    }

    void centered_text(const UiRectangle bounds, const std::string_view value,
            const int glyph_scale, const UiColor color, const double opacity,
            const int y_offset = 0, const std::span<const std::byte> font = {},
            const std::uint32_t pixel_size = 12,
            const std::string_view horizontal_alignment = "center") {
        if (value.empty())
            return;
        const auto raster =
                font.empty() ? ui_rasterize_builtin_text(value, 2,
                                       static_cast<float>(pixel_size) / 12.0F)
                             : ui_rasterize_font_text(value, font, pixel_size);
        if (raster) {
            auto origin_x = bounds.x + (bounds.width - raster->width) / 2;
            if (horizontal_alignment == "left")
                origin_x = bounds.x;
            else if (horizontal_alignment == "right")
                origin_x = bounds.x + bounds.width - raster->width;
            const auto origin_y =
                    bounds.y + (bounds.height - raster->height) / 2 + y_offset;
            for (auto y = 0; y < raster->height; ++y)
                for (auto x = 0; x < raster->width; ++x) {
                    const auto coverage =
                            raster->alpha[static_cast<std::size_t>(y) *
                                                  raster->width +
                                          x];
                    if (coverage != 0)
                        pixel(origin_x + x, origin_y + y, color,
                                opacity * coverage / 255.0);
                }
            return;
        }
        const auto advance = 6 * glyph_scale;
        const auto width = std::max(
                static_cast<int>(value.size()) * advance - glyph_scale, 0);
        auto x = bounds.x + (bounds.width - width) / 2;
        if (horizontal_alignment == "left")
            x = bounds.x;
        else if (horizontal_alignment == "right")
            x = bounds.x + bounds.width - width;
        const auto y =
                bounds.y + (bounds.height - 7 * glyph_scale) / 2 + y_offset;
        for (const unsigned char character : value) {
            const auto glyph = ui_fallback_glyph_rows(character);
            for (auto row = 0; row < 7; ++row)
                for (auto column = 0; column < 5; ++column)
                    if ((glyph[row] >> (4 - column)) & 1U)
                        rectangle({ x + column * glyph_scale,
                                          y + row * glyph_scale, glyph_scale,
                                          glyph_scale },
                                color, opacity);
            x += advance;
        }
    }

  private:
    void pixel(const int x, const int y, UiColor color, const double opacity) {
        if (x < 0 || y < 0 || x >= static_cast<int>(target_frame.width) ||
                y >= static_cast<int>(target_frame.height))
            return;
        const auto offset = static_cast<std::size_t>(y) * target_frame.stride +
                            static_cast<std::size_t>(x) * 4;
        const auto alpha = static_cast<unsigned>(std::clamp(
                static_cast<double>(color.alpha) * opacity, 0.0, 255.0));
        const auto inverse = 255U - alpha;
        target_frame.rgba[offset] = static_cast<std::uint8_t>(
                (color.red * alpha + target_frame.rgba[offset] * inverse) /
                255U);
        target_frame.rgba[offset + 1] = static_cast<std::uint8_t>(
                (color.green * alpha +
                        target_frame.rgba[offset + 1] * inverse) /
                255U);
        target_frame.rgba[offset + 2] = static_cast<std::uint8_t>(
                (color.blue * alpha + target_frame.rgba[offset + 2] * inverse) /
                255U);
        target_frame.rgba[offset + 3] = 255;
    }

    UiFrame &target_frame;
};

class NativeLayout {
  public:
    NativeLayout(const UiDocument &document, const UiAssetProvider *assets,
            UiFrame &frame, const float scale,
            std::vector<NativeUiHitRegion> *hits = nullptr)
        : document(document), assets(assets), canvas(frame), scale(scale),
          hits(hits) {
        if (assets)
            native_font =
                    assets->data("fonts/Noto_Sans/NotoSans-ExtraLight.ttf");
        if (!native_font)
            native_font = platform_ui_font();
        const auto collect_fonts =
                [&](const auto &self, const NativeUiComponentNode &node,
                        std::shared_ptr<const std::vector<std::byte>> font)
                -> void {
            if (assets)
                if (const auto *family = outer_modifier(node, "font_family")) {
                    const auto path =
                            text(family->arguments, "#1").value_or("");
                    if (!path.empty())
                        if (auto loaded = assets->data(path))
                            font = std::move(loaded);
                }
            node_fonts.emplace(&node, font);
            for (const auto &child : node.children)
                self(self, child, font);
        };
        for (const auto &root : document.native_ui_component_roots)
            collect_fonts(collect_fonts, root, native_font);
    }

    void render() {
        if (hits)
            hits->clear();
        root = { 0, 0, static_cast<int>(std::lround(document.width * scale)),
            static_cast<int>(std::lround(document.height * scale)) };
        for (const auto &node : document.native_ui_component_roots)
            draw(node, root, 1.0, { 205, 205, 205, 255 });
        // the popover doesn't count for the anchor's size, and paint it last so
        // it stays on top
        for (std::size_t i = 0; i < popovers.size(); ++i) {
            const auto popup = popovers[i];
            const auto size = measure(*popup.content);
            UiRectangle bounds{ popup.anchor.x, popup.anchor.y,
                static_cast<int>(std::lround(size.width * scale)),
                static_cast<int>(std::lround(size.height * scale)) };
            const auto direction = text(popup.modifier->arguments, "direction")
                                           .value_or("down");
            const auto alignment = text(popup.modifier->arguments, "alignment")
                                           .value_or("center");
            const auto spacing = static_cast<int>(std::lround(
                    number(popup.modifier->arguments, "spacing").value_or(0) *
                    scale));
            const auto offset = [&](int anchor, int content) {
                return alignment == "start" ? 0
                       : alignment == "end" ? anchor - content
                                            : (anchor - content) / 2;
            };
            if (direction == "up" || direction == "down") {
                bounds.x += offset(popup.anchor.width, bounds.width);
                bounds.y += direction == "up" ? -bounds.height - spacing
                                              : popup.anchor.height + spacing;
            } else {
                bounds.y += offset(popup.anchor.height, bounds.height);
                bounds.x += direction == "left" ? -bounds.width - spacing
                                                : popup.anchor.width + spacing;
            }
            // if it overflows try the other side, take whichever shows more and
            // clamp
            const auto adjust = [](int position, int length, int limit,
                                        int anchor_position,
                                        int anchor_length) {
                if (position < 0 || position + length > limit) {
                    const auto mirrored = 2 * anchor_position + anchor_length -
                                          position - length;
                    const auto visible = [&](int start) {
                        return std::max(0, std::min(start + length, limit) -
                                                   std::max(start, 0));
                    };
                    if (visible(mirrored) > visible(position))
                        position = mirrored;
                }
                position = std::max(position, 0);
                if (position + length > limit)
                    position = limit - length;
                return position;
            };
            bounds.x = adjust(bounds.x, bounds.width, root.width,
                    popup.anchor.x, popup.anchor.width);
            bounds.y = adjust(bounds.y, bounds.height, root.height,
                    popup.anchor.y, popup.anchor.height);
            if (hits && popup.enabled) {
                const auto close = std::ranges::find(popup.modifier->actions,
                        std::string_view{ "on_auto_close" },
                        &NativeUiModifier::Action::phase);
                const auto trigger =
                        text(popup.modifier->arguments, "auto_close_when")
                                .value_or("");
                if (close != popup.modifier->actions.end() &&
                        (trigger == "pressed outside anchor" ||
                                trigger == "pressed outside")) {
                    std::vector<UiRectangle> outside{ root };
                    std::vector<UiRectangle> exclusions{ bounds };
                    if (trigger == "pressed outside anchor")
                        exclusions.push_back(popup.anchor);
                    for (const auto excluded : exclusions) {
                        std::vector<UiRectangle> remaining;
                        for (const auto rect : outside) {
                            const auto left = std::max(rect.x, excluded.x);
                            const auto top = std::max(rect.y, excluded.y);
                            const auto right = std::min(rect.x + rect.width,
                                    excluded.x + excluded.width);
                            const auto bottom = std::min(rect.y + rect.height,
                                    excluded.y + excluded.height);
                            if (left >= right || top >= bottom) {
                                remaining.push_back(rect);
                                continue;
                            }
                            for (const auto piece : {
                                         UiRectangle{ rect.x, rect.y,
                                                 rect.width, top - rect.y },
                                         UiRectangle{ rect.x, bottom,
                                                 rect.width,
                                                 rect.y + rect.height -
                                                         bottom },
                                         UiRectangle{ rect.x, top,
                                                 left - rect.x, bottom - top },
                                         UiRectangle{ right, top,
                                                 rect.x + rect.width - right,
                                                 bottom - top } })
                                if (piece.width > 0 && piece.height > 0)
                                    remaining.push_back(piece);
                        }
                        outside = std::move(remaining);
                    }
                    for (const auto rect : outside)
                        overlay_hit(rect, { { "start", close->id } });
                }
                // the padding must not click through
                overlay_hit(bounds, {});
            }
            draw(*popup.content, bounds, popup.opacity, popup.foreground,
                    "Popover", popup.enabled);
        }
    }

  private:
    struct Popover {
        const NativeUiComponentNode *content;
        const NativeUiModifier *modifier;
        UiRectangle anchor;
        double opacity;
        UiColor foreground;
        bool enabled;
    };
    void overlay_hit(const UiRectangle bounds,
            std::vector<NativeUiModifier::Action> actions) {
        NativeUiHitRegion hit;
        hit.x = static_cast<int>(std::lround(bounds.x / scale));
        hit.y = static_cast<int>(std::lround(bounds.y / scale));
        hit.width = static_cast<int>(std::lround(bounds.width / scale));
        hit.height = static_cast<int>(std::lround(bounds.height / scale));
        hit.tap = true;
        hit.actions = std::move(actions);
        hits->push_back(std::move(hit));
    }
    struct MeasureKey {
        const NativeUiComponentNode *node{};
        std::string_view parent_kind;
        std::size_t modifier_count{};
        bool operator==(const MeasureKey &) const = default;
    };

    std::shared_ptr<const std::vector<std::byte>> text_font(
            const NativeUiComponentNode &node) const {
        if (const auto found = node_fonts.find(&node);
                found != node_fonts.end())
            return found->second;
        return native_font;
    }
    struct MeasureKeyHash {
        std::size_t operator()(const MeasureKey &key) const {
            auto result = std::hash<const void *>{}(key.node);
            result ^= std::hash<std::string_view>{}(key.parent_kind) +
                      0x9e3779b9U + (result << 6U) + (result >> 2U);
            result ^= std::hash<std::size_t>{}(key.modifier_count) +
                      0x9e3779b9U + (result << 6U) + (result >> 2U);
            return result;
        }
    };

    Size measure(const NativeUiComponentNode &node,
            const std::string_view parent_kind = {},
            const std::size_t modifier_count =
                    std::numeric_limits<std::size_t>::max()) const {
        const MeasureKey key{ &node, parent_kind, modifier_count };
        if (const auto found = measure_cache.find(key);
                found != measure_cache.end())
            return found->second;
        // hidden still takes its space
        Size explicit_size{};
        const auto kind = layout_kind(node);
        if (kind == "Spacer") {
            // 4 units by default, zero on the cross axis and unbounded along
            // the stack
            const auto minimum =
                    number(node.properties, "min_length").value_or(4.0);
            if (explicit_size.width <= 0.0)
                explicit_size.width =
                        node.modifiers.empty() && parent_kind == "VStack"
                                ? 0.0
                                : minimum;
            if (explicit_size.height <= 0.0)
                explicit_size.height =
                        node.modifiers.empty() && parent_kind == "HStack"
                                ? 0.0
                                : minimum;
        }
        if (kind == "Image" && assets) {
            if (const auto file = text(node.properties, "file"))
                if (const auto image = assets->image(*file, scale)) {
                    const auto density = std::max(image->pixel_density, 1.0F);
                    if (explicit_size.width <= 0.0)
                        explicit_size.width = image->width / density;
                    if (explicit_size.height <= 0.0)
                        // one row, not the whole strip
                        explicit_size.height =
                                image->height / density /
                                std::max(std::lround(number(
                                                 node.properties, "row_count")
                                                         .value_or(1.0)),
                                        1L);
                }
        }
        if (kind == "CircleToggle") {
            const auto image_size =
                    number(node.properties, "image_size").value_or(16.0);
            if (explicit_size.width <= 0.0)
                explicit_size.width = image_size;
            if (explicit_size.height <= 0.0)
                explicit_size.height = image_size;
        } else if (kind == "RingKnob") {
            const auto ring_size =
                    number(node.properties, "ring_size").value_or(51.0);
            if (explicit_size.width <= 0.0)
                explicit_size.width = ring_size;
            if (explicit_size.height <= 0.0)
                explicit_size.height = ring_size;
        } else if (kind == "KnobWithPower") {
            // ring plus the label under it, if we leave it at zero the HStack
            // collapses
            if (explicit_size.width <= 0.0)
                explicit_size.width = 30.0;
            if (explicit_size.height <= 0.0)
                explicit_size.height = 30.0;
        } else if (kind == "Slider") {
            // unframed sliders still need a width
            if (explicit_size.width <= 0.0)
                explicit_size.width = 145.0;
            if (explicit_size.height <= 0.0)
                explicit_size.height = 18.0;
        } else if (kind == "SelectorWithPopupCustomBg") {
            if (explicit_size.width <= 0.0)
                explicit_size.width = 120.0;
            if (explicit_size.height <= 0.0)
                explicit_size.height = 20.0;
        } else if (kind == "Ring") {
            // 2*thickness+6 on each axis, a missing frame side is flexible and
            // NOT the same as the other side
            const auto minimum =
                    number(node.properties, "thickness").value_or(4.0) * 2.0 +
                    6.0;
            if (explicit_size.width <= 0.0)
                explicit_size.width = minimum;
            if (explicit_size.height <= 0.0)
                explicit_size.height = minimum;
        }
        if (kind == "Text") {
            const auto value = text(node.properties, "text").value_or("");
            const auto font_size = argument(modifier(node, "font_size"), "size")
                                           .value_or(12.0);
            const auto font = text_font(node);
            const auto raster =
                    font ? ui_rasterize_font_text(value,
                                   std::span<const std::byte>{ *font },
                                   static_cast<std::uint32_t>(std::max(
                                           static_cast<long>(font_size), 1L)))
                         : ui_rasterize_builtin_text(value, 2,
                                   static_cast<float>(font_size / 12.0));
            if (raster) {
                if (explicit_size.width <= 0.0)
                    explicit_size.width = raster->width;
                if (explicit_size.height <= 0.0)
                    explicit_size.height = raster->height;
            } else {
                if (explicit_size.width <= 0.0)
                    explicit_size.width = value.size() * font_size * 0.6;
                if (explicit_size.height <= 0.0)
                    explicit_size.height = font_size;
            }
        }
        const auto spacing =
                number(node.properties, "spacing")
                        .value_or(short_kind(node) == "List" ? 2.0 : 0.0);
        Size content{};
        std::vector<const NativeUiComponentNode *> layout_children;
        append_layout_children(node, layout_children);
        for (const auto *child : layout_children) {
            auto child_size = measure(*child, kind);
            if (kind == "HStack") {
                if (content.width > 0.0 && child_size.width > 0.0)
                    content.width += spacing;
                content.width += child_size.width;
                content.height = std::max(content.height, child_size.height);
            } else if (kind == "VStack") {
                if (content.height > 0.0 && child_size.height > 0.0)
                    content.height += spacing;
                content.height += child_size.height;
                content.width = std::max(content.width, child_size.width);
            } else {
                content.width = std::max(content.width, child_size.width);
                content.height = std::max(content.height, child_size.height);
            }
        }
        if (explicit_size.width <= 0.0)
            explicit_size.width = content.width;
        if (explicit_size.height <= 0.0)
            explicit_size.height = content.height;
        const auto result =
                apply_layout_modifiers(node, explicit_size, modifier_count);
        measure_cache.emplace(key, result);
        return result;
    }

    AxisConstraints axis_constraints(const NativeUiComponentNode &node,
            const bool horizontal, const std::string_view parent_kind = {},
            const std::size_t modifier_count =
                    std::numeric_limits<std::size_t>::max()) const {
        const auto intrinsic = measure(node, parent_kind, modifier_count);
        const auto modifiers = std::span{ node.modifiers }.first(
                std::min(modifier_count, node.modifiers.size()));
        bool aligned_outside_frame = false;
        for (const auto &value : modifiers) {
            if (value.name == "frame" &&
                    number(value.arguments, horizontal ? "width" : "height"))
                aligned_outside_frame = false;
            else if (value.name == "align") {
                const auto axes = parse_alignment(
                        text(value.arguments, "#1").value_or(""));
                if (horizontal ? !axes.horizontal.empty()
                               : !axes.vertical.empty())
                    aligned_outside_frame = true;
            }
        }
        const auto has_frame_dimension =
                std::ranges::any_of(modifiers, [&](const auto &value) {
                    return value.name == "frame" &&
                           number(value.arguments,
                                   horizontal ? "width" : "height")
                                   .has_value();
                });
        if (has_frame_dimension && !aligned_outside_frame) {
            // the outer size, padding included
            const auto outer = horizontal ? intrinsic.width : intrinsic.height;
            return { outer, outer, 0 };
        }

        const auto infinity = std::numeric_limits<double>::infinity();
        const auto kind = layout_kind(node);
        auto minimum = horizontal ? intrinsic.width : intrinsic.height;
        auto maximum = minimum;

        if (kind == "Rectangle") {
            maximum = infinity;
        } else if (kind == "Spacer") {
            const bool expands =
                    !node.modifiers.empty() ||
                    (parent_kind != "HStack" && parent_kind != "VStack") ||
                    (horizontal ? parent_kind == "HStack"
                                : parent_kind == "VStack");
            maximum = expands ? infinity : minimum;
        } else if (kind == "HStack" || kind == "VStack" || kind == "ZStack") {
            const bool main_axis = horizontal == (kind == "HStack");
            double child_maximum{};
            bool unbounded{};
            std::size_t child_count{};
            std::vector<const NativeUiComponentNode *> layout_children;
            append_layout_children(node, layout_children);
            for (const auto *child : layout_children) {
                const auto child_constraints =
                        axis_constraints(*child, horizontal, kind);
                ++child_count;
                if (std::isinf(child_constraints.maximum))
                    unbounded = true;
                if (main_axis)
                    child_maximum += child_constraints.maximum;
                else
                    child_maximum =
                            std::max(child_maximum, child_constraints.maximum);
            }
            if (main_axis && child_count > 1) {
                const auto spacing =
                        number(node.properties, "spacing")
                                .value_or(
                                        short_kind(node) == "List" ? 2.0 : 0.0);
                child_maximum += spacing * (child_count - 1);
            }
            maximum = unbounded ? infinity : std::max(minimum, child_maximum);
        }

        for (const auto &value : modifiers) {
            if (value.name == "align" &&
                    !(short_kind(node) == "Image" &&
                            !flag(node.properties, "resizable"))) {
                const auto alignment = parse_alignment(
                        text(value.arguments, "#1").value_or(""));
                if (horizontal ? !alignment.horizontal.empty()
                               : !alignment.vertical.empty())
                    maximum = infinity;
            }
        }

        const auto limit_name = horizontal ? "max_width" : "max_height";
        for (const auto &value : modifiers)
            if (value.name == "frame")
                if (const auto limit = number(value.arguments, limit_name))
                    maximum = std::max(minimum, *limit);
        return { minimum, maximum, 0 };
    }

    static bool tight_constraints(const AxisConstraints &value) {
        if (std::isinf(value.maximum))
            return false;
        const auto difference = std::abs(value.minimum - value.maximum);
        const auto magnitude =
                std::min(std::abs(value.minimum), std::abs(value.maximum));
        return difference * 1.0e12 <= magnitude;
    }

    UiRectangle place(const NativeUiComponentNode &node,
            const UiRectangle parent, Size desired) const {
        const auto framed_axis = [&](const std::string_view axis) {
            return std::ranges::any_of(node.modifiers, [&](const auto &value) {
                return value.name == "frame" &&
                       number(value.arguments, axis).has_value();
            });
        };
        if (desired.width <= 0.0 && !framed_axis("width"))
            desired.width = parent.width / scale;
        if (desired.height <= 0.0 && !framed_axis("height"))
            desired.height = parent.height / scale;
        UiRectangle result{ parent.x, parent.y,
            static_cast<int>(std::lround(desired.width * scale)),
            static_cast<int>(std::lround(desired.height * scale)) };
        std::string_view alignment;
        // alignment in a Frame moves the content, not the Frame
        for (const auto &value : node.modifiers) {
            if (value.name == "frame")
                alignment = {};
            else if (value.name == "align")
                alignment = text(value.arguments, "#1").value_or(alignment);
        }
        const auto axes = parse_alignment(alignment);
        const bool centered_modifier_overlay =
                alignment.empty() && node.role.starts_with("modifier:");
        if (centered_modifier_overlay)
            result.x = parent.x + (parent.width - result.width) / 2;
        else if (axes.horizontal == "right")
            result.x = parent.x + parent.width - result.width;
        else if (axes.horizontal == "center")
            result.x = parent.x + (parent.width - result.width) / 2;
        if (centered_modifier_overlay)
            result.y = parent.y + (parent.height - result.height) / 2;
        else if (axes.vertical == "bottom")
            result.y = parent.y + parent.height - result.height;
        else if (axes.vertical == "center")
            result.y = parent.y + (parent.height - result.height) / 2;
        // local to the parent's content box, centering first pushes XyPad
        // indicators off the chart
        if (const auto *value = modifier(node, "position")) {
            result.x = parent.x +
                       static_cast<int>(std::lround(
                               number(value->arguments, "x").value_or(0.0) *
                               scale));
            result.y = parent.y +
                       static_cast<int>(std::lround(
                               number(value->arguments, "y").value_or(0.0) *
                               scale));
        }
        return result;
    }

    UiRectangle unwrap_layout_modifiers(const NativeUiComponentNode &node,
            UiRectangle bounds, const std::string_view parent_kind = {},
            const std::size_t begin = 0) const {
        for (std::size_t index = node.modifiers.size(); index-- > begin;) {
            const auto &value = node.modifiers[index];
            if (value.name == "padding") {
                const auto inset = padding(value);
                const auto left =
                        static_cast<int>(std::lround(inset.left * scale));
                const auto right =
                        static_cast<int>(std::lround(inset.right * scale));
                const auto top =
                        static_cast<int>(std::lround(inset.top * scale));
                const auto bottom =
                        static_cast<int>(std::lround(inset.bottom * scale));
                bounds.x += left;
                bounds.y += top;
                bounds.width = std::max(bounds.width - left - right, 0);
                bounds.height = std::max(bounds.height - top - bottom, 0);
            } else if (value.name == "offset") {
                bounds.x += static_cast<int>(std::lround(
                        number(value.arguments, "x").value_or(0.0) * scale));
                bounds.y += static_cast<int>(std::lround(
                        number(value.arguments, "y").value_or(0.0) * scale));
            } else if (value.name == "frame") {
                const auto child = measure(node, parent_kind, index);
                const auto has_flexible_child = [&](const bool horizontal) {
                    return std::ranges::any_of(
                            node.children, [horizontal](const auto &candidate) {
                                return !candidate.role.starts_with(
                                               "modifier:") &&
                                       flexible_axis(candidate, horizontal);
                            });
                };
                const auto prefix_has_frame = [&](const bool horizontal) {
                    for (std::size_t prefix = 0; prefix < index; ++prefix) {
                        const auto &candidate = node.modifiers[prefix];
                        if (candidate.name == "frame" &&
                                number(candidate.arguments,
                                        horizontal ? "width" : "height"))
                            return true;
                    }
                    return false;
                };
                const auto prefix_alignment = [&] {
                    Alignment result;
                    for (std::size_t prefix = 0; prefix < index; ++prefix) {
                        const auto &candidate = node.modifiers[prefix];
                        if (candidate.name != "align")
                            continue;
                        const auto parsed = parse_alignment(
                                text(candidate.arguments, "#1").value_or(""));
                        if (!parsed.horizontal.empty())
                            result.horizontal = parsed.horizontal;
                        if (!parsed.vertical.empty())
                            result.vertical = parsed.vertical;
                    }
                    return result;
                }();
                const bool intrinsic_image =
                        short_kind(node) == "Image" &&
                        !flag(node.properties, "resizable");
                const bool flexible_width =
                        !prefix_has_frame(true) &&
                        (child.width <= 0.0 || short_kind(node) == "Ring" ||
                                short_kind(node) == "Spacer" ||
                                (short_kind(node) == "Image" &&
                                        flag(node.properties, "resizable")) ||
                                (!intrinsic_image &&
                                        !prefix_alignment.horizontal.empty()) ||
                                aligned_overlay_requires_proposal(node, true) ||
                                descendant_flexible_axis(node, true) ||
                                has_flexible_child(true));
                const bool flexible_height =
                        !prefix_has_frame(false) &&
                        (child.height <= 0.0 || short_kind(node) == "Ring" ||
                                short_kind(node) == "Spacer" ||
                                (short_kind(node) == "Image" &&
                                        flag(node.properties, "resizable")) ||
                                (!intrinsic_image &&
                                        !prefix_alignment.vertical.empty()) ||
                                aligned_overlay_requires_proposal(
                                        node, false) ||
                                descendant_flexible_axis(node, false) ||
                                has_flexible_child(false));
                // the child can be bigger than the frame and end up with a
                // negative offset, don't clamp it
                const auto measured_width =
                        static_cast<int>(std::lround(child.width * scale));
                const auto measured_height =
                        static_cast<int>(std::lround(child.height * scale));
                const bool resizable_image = short_kind(node) == "Image" &&
                                             flag(node.properties, "resizable");
                auto child_width =
                        flexible_width
                                ? (resizable_image ? bounds.width
                                                   : std::max(bounds.width,
                                                             measured_width))
                                : measured_width;
                const auto child_height =
                        flexible_height
                                ? (resizable_image ? bounds.height
                                                   : std::max(bounds.height,
                                                             measured_height))
                                : measured_height;
                const auto width_constraints =
                        axis_constraints(node, true, parent_kind, index);
                if (std::isfinite(width_constraints.maximum) &&
                        width_constraints.maximum > width_constraints.minimum)
                    child_width = std::max(child_width,
                            static_cast<int>(std::lround(
                                    std::min<double>(bounds.width / scale,
                                            width_constraints.maximum) *
                                    scale)));
                const auto alignment = parse_alignment(
                        text(value.arguments, "alignment").value_or("center"));
                if (alignment.horizontal == "right")
                    bounds.x += bounds.width - child_width;
                else if (alignment.horizontal == "center")
                    bounds.x += (bounds.width - child_width) / 2;
                if (alignment.vertical == "bottom")
                    bounds.y += bounds.height - child_height;
                else if (alignment.vertical == "center")
                    bounds.y += (bounds.height - child_height) / 2;
                bounds.width = child_width;
                bounds.height = child_height;
            }
        }
        return bounds;
    }

    void draw(const NativeUiComponentNode &node, const UiRectangle offered,
            const double inherited_opacity, const UiColor inherited_foreground,
            const std::string_view parent_kind = {},
            const bool inherited_enabled = true) {
        if (hidden(node))
            return;
        // disabled descendants keep their paint and layout, but aren't
        // pointer or hover targets
        const auto enabled =
                inherited_enabled &&
                !std::ranges::any_of(node.modifiers, [](const auto &value) {
                    return value.name == "disabled" &&
                           flag(value.arguments, "#1");
                });
        auto desired_size = measure(node, parent_kind);
        const auto primitive_kind = short_kind(node);
        const auto aligned = modifier(node, "align") != nullptr;
        const auto layout = layout_kind(node);
        const auto flexible_stack_width =
                layout == "HStack" && descendant_flexible_axis(node, true);
        const auto flexible_stack_height =
                layout == "VStack" && descendant_flexible_axis(node, false);
        const auto modifier_child = node.role.starts_with("modifier:");
        // still takes the space up to its max, it just must not make the
        // parents unbounded
        if (!modifier_child) {
            const auto width_constraints =
                    axis_constraints(node, true, parent_kind);
            if (std::isfinite(width_constraints.maximum) &&
                    width_constraints.maximum > width_constraints.minimum)
                desired_size.width = std::max(desired_size.width,
                        std::min<double>(offered.width / scale,
                                width_constraints.maximum));
        }
        // keep the proposal until unwrap_layout_modifiers, otherwise aligned
        // text in a width-only frame collapses to the top
        if (aligned && modifier(node, "frame")) {
            const auto fixed = frame_size(node);
            if (fixed.width <= 0.0 && flexible_axis(node, true))
                desired_size.width = offered.width / scale;
            if (fixed.height <= 0.0 && flexible_axis(node, false))
                desired_size.height = offered.height / scale;
        }
        if (primitive_kind == "Spacer") {
            // only a bare Spacer straight in a stack is zero on the cross axis
            const auto fixed = frame_size(node);
            if (fixed.width <= 0.0 &&
                    (!node.modifiers.empty() || parent_kind != "VStack"))
                desired_size.width = std::max(desired_size.width,
                        static_cast<double>(offered.width / scale));
            if (fixed.height <= 0.0 &&
                    (!node.modifiers.empty() || parent_kind != "HStack"))
                desired_size.height = std::max(desired_size.height,
                        static_cast<double>(offered.height / scale));
        }
        // background/overlay get the whole view's bounds, sizing them to the
        // content first centers stuff that should sit on an edge
        if (modifier_child && layout == "ZStack") {
            desired_size.width = offered.width / scale;
            desired_size.height = offered.height / scale;
        }
        // keep the proposal, otherwise the whole thing gets aligned at its
        // minimum width
        const auto intrinsic_width =
                aligned && !modifier_child &&
                !descendant_aligned_overlay_requires_proposal(node, true) &&
                !flexible_stack_width && !descendant_flexible_axis(node, true);
        const auto intrinsic_height =
                aligned && !modifier_child &&
                !descendant_aligned_overlay_requires_proposal(node, false) &&
                !flexible_stack_height &&
                !descendant_flexible_axis(node, false);
        if (!intrinsic_width &&
                (primitive_kind == "Rectangle" ||
                        descendant_flexible_axis(node, true)) &&
                !frame_limit(node, "width"))
            desired_size.width = offered.width / scale;
        if (!intrinsic_height &&
                (primitive_kind == "Rectangle" ||
                        descendant_flexible_axis(node, false)) &&
                !frame_limit(node, "height"))
            desired_size.height = offered.height / scale;
        // a max-sized frame takes the whole proposal even if the Text inside
        // doesn't, keep that width before unwrapping
        if (const auto maximum = frame_limit(node, "max_width");
                maximum && !frame_limit(node, "width"))
            desired_size.width = std::min(*maximum,
                    std::max(desired_size.width,
                            std::min<double>(offered.width / scale, *maximum)));
        if (const auto maximum = frame_limit(node, "max_height");
                maximum && !frame_limit(node, "height"))
            desired_size.height = std::min(
                    *maximum, std::max(desired_size.height,
                                      std::min<double>(offered.height / scale,
                                              *maximum)));
        if (primitive_kind == "Ring") {
            // max(min(width, height), 2*thickness+6), AFTER the proposal comes
            // down or we lose the outer frame
            const auto minimum =
                    number(node.properties, "thickness").value_or(4.0) * 2.0 +
                    6.0;
            const auto fixed = frame_size(node);
            if (fixed.width <= 0.0)
                desired_size.width = offered.width / scale;
            if (fixed.height <= 0.0)
                desired_size.height = offered.height / scale;
            const auto side = std::max(
                    std::min(desired_size.width, desired_size.height), minimum);
            desired_size = { side, side };
        }
        const auto bounds = place(node, offered, desired_size);
        const auto primitive_bounds =
                unwrap_layout_modifiers(node, bounds, parent_kind);
        if (hits && enabled) {
            const auto *control = property(node.properties, "control_id");
            const auto *control_name =
                    control ? std::get_if<std::string>(&control->value)
                            : nullptr;
            const auto component = short_kind(node);
            const auto *tap_modifier = modifier(node, "on_tap_gesture");
            const auto *drag_modifier = modifier(node, "on_drag_gesture");
            const bool tap = tap_modifier != nullptr;
            const bool drag = drag_modifier != nullptr ||
                              component == "DynamicsKnob" ||
                              component == "RingKnob" || component == "Knob";
            std::vector<NativeUiModifier::Action> actions;
            for (const auto &candidate : node.modifiers)
                if (candidate.name == "on_tap_gesture" ||
                        candidate.name == "on_drag_gesture" ||
                        candidate.name == "on_hover_gesture")
                    actions.insert(actions.end(), candidate.actions.begin(),
                            candidate.actions.end());
            auto hit_bounds = actions.empty() ? primitive_bounds : bounds;
            if (!actions.empty()) {
                for (std::size_t index = node.modifiers.size(); index-- > 0;) {
                    const auto &candidate = node.modifiers[index];
                    if (candidate.name != "on_tap_gesture" &&
                            candidate.name != "on_drag_gesture" &&
                            candidate.name != "on_hover_gesture")
                        continue;
                    hit_bounds = unwrap_layout_modifiers(
                            node, bounds, parent_kind, index + 1);
                    break;
                }
            }
            if (((control_name && !control_name->empty()) ||
                        !actions.empty()) &&
                    hit_bounds.width > 0 && hit_bounds.height > 0) {
                NativeUiHitRegion hit;
                hit.x = static_cast<std::int32_t>(
                        std::lround(hit_bounds.x / scale));
                hit.y = static_cast<std::int32_t>(
                        std::lround(hit_bounds.y / scale));
                hit.width = static_cast<std::int32_t>(
                        std::lround(hit_bounds.width / scale));
                hit.height = static_cast<std::int32_t>(
                        std::lround(hit_bounds.height / scale));
                if (control_name)
                    hit.control_id = *control_name;
                hit.from = number(node.properties, "from").value_or(0.0);
                hit.to = number(node.properties, "to").value_or(1.0);
                hit.value = number(node.properties, "value").value_or(0.0);
                hit.step_size =
                        number(node.properties, "step_size").value_or(0.0);
                hit.range_factor =
                        number(node.properties, "range_factor").value_or(1.0);
                hit.selection_count =
                        (component == "List" || component.ends_with("List"))
                                ? node.children.size()
                                : 0U;
                hit.actions = std::move(actions);
                if (drag_modifier)
                    hit.drag_threshold = argument(drag_modifier, "threshold");
                hit.tap = tap;
                hit.drag = drag;
                hits->push_back(std::move(hit));
            }
        }
        const auto combined_opacity = inherited_opacity * opacity(node);
        auto native_foreground = inherited_foreground;
        if (const auto *foreground = modifier(node, "foreground_color"))
            if (const auto value = color(foreground->arguments, "#1"))
                native_foreground = *value;
        const auto raw_kind = short_kind(node);
        const auto kind = layout_kind(node);
        const auto modifier_offer = [&](const NativeUiComponentNode &child,
                                            const std::string_view name) {
            auto result = bounds;
            const auto prefix =
                    std::string{ "modifier:" } + std::string{ name } + ":";
            const NativeUiModifier *wrapper{};
            if (child.role.starts_with(prefix)) {
                const auto suffix =
                        std::string_view(child.role).substr(prefix.size());
                std::size_t consumed{};
                try {
                    const auto index =
                            std::stoull(std::string(suffix), &consumed);
                    if (consumed > 0 && index > 0 &&
                            index <= node.modifiers.size() &&
                            node.modifiers[index - 1].name == name)
                        wrapper = &node.modifiers[index - 1];
                } catch (const std::exception &) {}
            }
            if (!wrapper)
                wrapper = modifier(node, name);
            if (!wrapper)
                return result;
            // everything after this modifier wraps the view too
            const auto wrapper_end =
                    static_cast<std::size_t>(wrapper - node.modifiers.data()) +
                    1;
            result = unwrap_layout_modifiers(
                    node, bounds, parent_kind, wrapper_end);
            const auto alignment_text =
                    text(wrapper->arguments, "alignment")
                            .value_or(text(wrapper->arguments, "#2")
                                            .value_or(""));
            const auto alignment = parse_alignment(alignment_text);
            if (alignment.horizontal.empty() && alignment.vertical.empty())
                return result;
            const auto intrinsic = measure(child);
            const auto width =
                    static_cast<int>(std::lround(intrinsic.width * scale));
            const auto height =
                    static_cast<int>(std::lround(intrinsic.height * scale));
            if (width > 0 && width < result.width) {
                if (alignment.horizontal == "right")
                    result.x += result.width - width;
                else if (alignment.horizontal == "center")
                    result.x += (result.width - width) / 2;
                result.width = width;
            }
            if (height > 0 && height < result.height) {
                if (alignment.vertical == "bottom")
                    result.y += result.height - height;
                else if (alignment.vertical == "center")
                    result.y += (result.height - height) / 2;
                result.height = height;
            }
            return result;
        };
        for (const auto &child : node.children)
            if (child.role.starts_with("modifier:background:"))
                draw(child, modifier_offer(child, "background"),
                        combined_opacity, native_foreground, {}, enabled);
        if (raw_kind == "Canvas" && !node.canvas_commands.empty()) {
            auto stroke = color(node.canvas_style, "stroke_style")
                                  .value_or(native_foreground);
            const auto width =
                    number(node.canvas_style, "line_width").value_or(1.0) *
                    scale;
            std::optional<std::pair<int, int>> cursor;
            for (const auto &command : node.canvas_commands) {
                const auto x = primitive_bounds.x +
                               static_cast<int>(std::lround(
                                       command.x * primitive_bounds.width));
                const auto y = primitive_bounds.y +
                               static_cast<int>(std::lround(
                                       command.y * primitive_bounds.height));
                if (command.kind == NativeUiCanvasCommand::Kind::move_to) {
                    cursor = std::pair{ x, y };
                } else if (cursor) {
                    canvas.line(cursor->first, cursor->second, x, y, width,
                            stroke, combined_opacity);
                    cursor = std::pair{ x, y };
                }
            }
        }
        if (raw_kind == "Image" && assets) {
            // the background is just another Image here, don't skip it because
            // background_picture is set
            if (const auto file = text(node.properties, "file"); file)
                if (const auto image = assets->image(*file, scale)) {
                    auto image_bounds = primitive_bounds;
                    const auto frame_count = std::ranges::count(node.modifiers,
                            std::string_view{ "frame" },
                            &NativeUiModifier::name);
                    // the first frame is the image's size, the last one is
                    // only for the parent's layout
                    if (frame_count > 1) {
                        const auto *inner_frame = modifier(node, "frame");
                        const auto inner_width =
                                argument(inner_frame, "width")
                                        .value_or(image->width /
                                                  std::max(image->pixel_density,
                                                          1.0F));
                        const auto inner_height =
                                argument(inner_frame, "height")
                                        .value_or(
                                                image->height /
                                                std::max(image->pixel_density,
                                                        1.0F) /
                                                std::max(
                                                        std::lround(number(
                                                                node.properties,
                                                                "row_count")
                                                                        .value_or(
                                                                                1.0)),
                                                        1L));
                        const auto inset = padding(node);
                        const UiRectangle outer_content{
                            bounds.x + static_cast<int>(
                                               std::lround(inset.left * scale)),
                            bounds.y + static_cast<int>(
                                               std::lround(inset.top * scale)),
                            bounds.width - static_cast<int>(std::lround(
                                                   (inset.left + inset.right) *
                                                   scale)),
                            bounds.height -
                                    static_cast<int>(std::lround(
                                            (inset.top + inset.bottom) * scale))
                        };
                        image_bounds.width = static_cast<int>(
                                std::lround(inner_width * scale));
                        image_bounds.height = static_cast<int>(
                                std::lround(inner_height * scale));
                        image_bounds.x = outer_content.x;
                        image_bounds.y =
                                outer_content.y +
                                (outer_content.height - image_bounds.height) /
                                        2;
                        if (const auto *alignment_modifier =
                                        modifier(node, "align")) {
                            const auto alignment = parse_alignment(
                                    text(alignment_modifier->arguments, "#1")
                                            .value_or(""));
                            if (alignment.horizontal == "center")
                                image_bounds.x += (outer_content.width -
                                                          image_bounds.width) /
                                                  2;
                            else if (alignment.horizontal == "right")
                                image_bounds.x += outer_content.width -
                                                  image_bounds.width;
                            if (alignment.vertical == "top")
                                image_bounds.y = outer_content.y;
                            else if (alignment.vertical == "bottom")
                                image_bounds.y = outer_content.y +
                                                 outer_content.height -
                                                 image_bounds.height;
                        }
                    }
                    if (!flag(node.properties, "resizable") &&
                            !modifier(node, "frame")) {
                        const auto density =
                                std::max(image->pixel_density, 1.0F);
                        image_bounds.width = static_cast<int>(
                                std::lround(image->width / density * scale));
                        // clang-format off
                        image_bounds.height =
                                static_cast<int>(std::lround(image->height /
                                density * scale /
                                std::max(std::lround(number(node.properties,
                                "row_count").value_or(1.0)), 1L)));
                        // clang-format on
                    }
                    // already aligned and offset, doing it again here throws
                    // the offset away
                    const auto row_count = std::max(
                            static_cast<int>(std::lround(
                                    number(node.properties, "row_count")
                                            .value_or(1.0))),
                            1);
                    const auto row = std::clamp(
                            static_cast<int>(
                                    std::lround(number(node.properties, "row")
                                                    .value_or(1.0))),
                            1, row_count);
                    const auto rotation =
                            argument(modifier(node, "rotation"), "angle")
                                    .value_or(0.0);
                    if (rotation != 0.0 && row_count == 1) {
                        // offset first, then rotate around the wrapper's
                        // center, NOT the bitmap's
                        canvas.rotated_image(*image, image_bounds, bounds,
                                rotation, combined_opacity);
                    } else if (row_count > 1) {
                        const auto row_height = std::max(
                                static_cast<int>(image->height) / row_count, 1);
                        canvas.image_region(*image, image_bounds,
                                { 0, (row - 1) * row_height,
                                        static_cast<int>(image->width),
                                        row_height },
                                combined_opacity);
                    } else {
                        double residual_x{}, residual_y{};
                        for (const auto &value : node.modifiers)
                            if (value.name == "offset") {
                                const auto x = number(value.arguments, "x")
                                                       .value_or(0.0) *
                                               scale;
                                const auto y = number(value.arguments, "y")
                                                       .value_or(0.0) *
                                               scale;
                                residual_x += x - std::lround(x);
                                residual_y += y - std::lround(y);
                            }
                        canvas.image(*image, image_bounds, combined_opacity,
                                residual_x, residual_y);
                    }
                }
        }
        if (raw_kind == "Rectangle") {
            // no color means transparent, NOT the text color, or we get white
            // boxes everywhere
            if (const auto rectangle_color = color(node.properties, "color");
                    rectangle_color && rectangle_color->alpha != 0) {
                const auto *frame = modifier(node, "frame");
                const auto explicit_zero_width =
                        frame &&
                        number(frame->arguments, "width").value_or(1.0) == 0.0;
                const auto explicit_zero_height =
                        frame &&
                        number(frame->arguments, "height").value_or(1.0) == 0.0;
                if (!explicit_zero_width && !explicit_zero_height)
                    canvas.rounded_rectangle(primitive_bounds,
                            number(node.properties, "corner_radius")
                                            .value_or(0.0) *
                                    scale,
                            *rectangle_color, combined_opacity);
            }
        } else if (raw_kind == "Ring") {
            const auto thickness =
                    number(node.properties, "thickness").value_or(4.0) * scale;
            const auto angle = number(node.properties, "angle").value_or(120.0);
            const auto rotation =
                    number(node.properties, "rotation").value_or(0.0) +
                    argument(modifier(node, "rotation"), "angle").value_or(0.0);
            canvas.ring(primitive_bounds, thickness, angle, rotation,
                    color(node.properties, "color").value_or(native_foreground),
                    combined_opacity);
        } else if (raw_kind == "Text") {
            const auto font = text_font(node);
            const auto value = text(node.properties, "text").value_or("");
            const auto font_size = argument(modifier(node, "font_size"), "size")
                                           .value_or(12.0);
            const auto glyph_scale = std::max(
                    static_cast<int>(std::lround(font_size * scale / 7.0)), 1);
            std::string_view horizontal_alignment = "center";
            if (const auto *alignment_modifier = modifier(node, "align"))
                horizontal_alignment = parse_alignment(
                        text(alignment_modifier->arguments, "#1").value_or(""))
                                               .horizontal;
            canvas.centered_text(primitive_bounds, value, glyph_scale,
                    native_foreground, combined_opacity, 0,
                    font ? std::span<const std::byte>{ *font }
                         : std::span<const std::byte>{},
                    static_cast<std::uint32_t>(std::max(
                            static_cast<long>(font_size) * scale, 1.0F)),
                    horizontal_alignment);
        } else if (raw_kind == "CircleToggle" && assets) {
            if (const auto file = text(node.properties, "image"))
                if (const auto image = assets->image(*file, scale)) {
                    const auto image_size = static_cast<int>(std::lround(
                            number(node.properties, "image_size")
                                    .value_or(image->width /
                                              std::max(image->pixel_density,
                                                      1.0F)) *
                            scale));
                    const UiRectangle image_bounds{
                        primitive_bounds.x +
                                (primitive_bounds.width - image_size) / 2,
                        primitive_bounds.y +
                                (primitive_bounds.height - image_size) / 2,
                        image_size, image_size
                    };
                    canvas.image(*image, image_bounds, combined_opacity);
                }
        } else if (raw_kind == "DynamicsKnob" || raw_kind == "RingKnob" ||
                   raw_kind == "Knob") {
            auto knob_bounds = primitive_bounds;
            if (const auto ring_size = number(node.properties, "ring_size")) {
                const auto pixels =
                        static_cast<int>(std::lround(*ring_size * scale));
                knob_bounds.x += (knob_bounds.width - pixels) / 2;
                knob_bounds.y += (knob_bounds.height - pixels) / 2;
                knob_bounds.width = pixels;
                knob_bounds.height = pixels;
            }
            canvas.ring(knob_bounds, std::max(2.0 * scale, 1.0), 360.0, 0.0,
                    native_foreground, combined_opacity);
        } else if (raw_kind == "KnobWithPower") {
            const auto ring_size = static_cast<int>(std::lround(30.0 * scale));
            UiRectangle knob_bounds{
                primitive_bounds.x + (primitive_bounds.width - ring_size) / 2,
                primitive_bounds.y + (primitive_bounds.height - ring_size) / 2,
                ring_size, ring_size
            };
            const auto knob_value =
                    std::clamp(number(node.properties, "__knob_control_value")
                                       .value_or(0.5),
                            0.0, 1.0);
            // the ring is always full, the indicator shows the value
            canvas.ring(knob_bounds, std::max(2.0 * scale, 1.0), 360.0, 0.0,
                    native_foreground, combined_opacity);
            const auto knob_center_x = knob_bounds.x + knob_bounds.width / 2;
            const auto knob_center_y = knob_bounds.y + knob_bounds.height / 2;
            const auto indicator_angle =
                    (knob_value * 270.0 + 45.0) * 0.017453292519943295;
            const auto indicator_distance = 9.0 * scale;
            // 2x10 bar centered in a 4x12 viewport
            const auto indicator_length = 10.0 * scale;
            const auto indicator_inner =
                    indicator_distance - indicator_length * 0.5;
            const auto indicator_outer =
                    indicator_distance + indicator_length * 0.5;
            const auto indicator_x0 =
                    knob_center_x +
                    static_cast<int>(std::lround(
                            std::sin(indicator_angle) * indicator_inner));
            const auto indicator_y0 =
                    knob_center_y -
                    static_cast<int>(std::lround(
                            std::cos(indicator_angle) * indicator_inner));
            const auto indicator_x1 =
                    knob_center_x +
                    static_cast<int>(std::lround(
                            std::sin(indicator_angle) * indicator_outer));
            const auto indicator_y1 =
                    knob_center_y -
                    static_cast<int>(std::lround(
                            std::cos(indicator_angle) * indicator_outer));
            canvas.line(indicator_x0, indicator_y0, indicator_x1, indicator_y1,
                    std::max(2.0 * scale, 1.0), native_foreground,
                    combined_opacity);
            const auto powered =
                    number(node.properties, "__power_control_value")
                            .value_or(1.0) != 0.0;
            // offset by (-19, -19)
            const auto power_center_x =
                    knob_center_x - static_cast<int>(std::lround(19.0 * scale));
            const auto power_center_y =
                    knob_center_y - static_cast<int>(std::lround(19.0 * scale));
            const auto power_opacity =
                    combined_opacity * (powered ? 1.0 : 0.35);
            // 10x11 mark in 18 units
            const auto glyph_width =
                    std::max(static_cast<int>(std::lround(10.0 * scale)), 1);
            const auto glyph_height =
                    std::max(static_cast<int>(std::lround(11.0 * scale)), 1);
            canvas.ring({ power_center_x - glyph_width / 2,
                                power_center_y - glyph_height / 2, glyph_width,
                                glyph_height },
                    std::max(1.25 * scale, 1.0), 320.0, 290.0,
                    native_foreground, power_opacity);
            // the stem is a filled rounded rect, x=8.1667..9.8333 y=3..8, a
            // stroked line comes out too long at the top
            const auto stem_width = std::max(
                    static_cast<int>(std::lround(1.6666667 * scale)), 1);
            const auto stem_height =
                    std::max(static_cast<int>(std::lround(5.0 * scale)), 1);
            canvas.rounded_rectangle(
                    { power_center_x - stem_width / 2,
                            power_center_y -
                                    static_cast<int>(std::lround(6.0 * scale)),
                            stem_width, stem_height },
                    0.4166667 * scale, native_foreground, power_opacity);
            const auto label = text(node.properties, "knob_label").value_or("");
            const auto label_height =
                    std::max(static_cast<int>(std::lround(14.0 * scale)), 1);
            const auto label_center_y =
                    knob_center_y + static_cast<int>(std::lround(28.0 * scale));
            canvas.centered_text(
                    { primitive_bounds.x, label_center_y - label_height / 2,
                            primitive_bounds.width, label_height },
                    label, 1, native_foreground, combined_opacity, 0,
                    native_font ? std::span<const std::byte>{ *native_font }
                                : std::span<const std::byte>{},
                    static_cast<std::uint32_t>(
                            std::max(static_cast<long>(11.0 * scale), 1L)));
        } else if (raw_kind == "Slider") {
            const auto center_y =
                    primitive_bounds.y + primitive_bounds.height / 2;
            const auto margin = static_cast<int>(std::lround(7.0 * scale));
            canvas.rectangle(
                    { primitive_bounds.x + margin, center_y,
                            std::max(primitive_bounds.width - margin * 2, 0),
                            std::max(static_cast<int>(std::lround(scale)), 1) },
                    native_foreground, combined_opacity * 0.55);
            auto normalized =
                    number(node.properties, "__control_value").value_or(0.5);
            if (normalized < 0.0 || normalized > 1.0)
                normalized = 0.5;
            const auto handle =
                    std::max(static_cast<int>(std::lround(14.0 * scale)), 2);
            const auto travel =
                    std::max(primitive_bounds.width - margin * 2 - handle, 0);
            const auto handle_x =
                    primitive_bounds.x + margin +
                    static_cast<int>(std::lround(travel * normalized));
            canvas.rounded_rectangle(
                    { handle_x, center_y - handle / 2, handle, handle },
                    handle / 2.0, native_foreground, combined_opacity);
            const auto label = text(node.properties, "label").value_or("");
            const auto label_height =
                    std::max(static_cast<int>(std::lround(15.0 * scale)), 1);
            canvas.centered_text(
                    { primitive_bounds.x,
                            primitive_bounds.y + primitive_bounds.height,
                            primitive_bounds.width, label_height },
                    label, 1, native_foreground, combined_opacity, 0,
                    native_font ? std::span<const std::byte>{ *native_font }
                                : std::span<const std::byte>{},
                    static_cast<std::uint32_t>(
                            std::max(static_cast<long>(10.0 * scale), 1L)));
        } else if (raw_kind == "SelectorWithPopupCustomBg") {
            const auto style = text(node.properties, "style").value_or("");
            auto value = text(node.properties, "text").value_or("");
            if (value.empty() && style == "claire_special_footer")
                value = "Soft Mallet";
            else if (value.empty() && style == "far_mic")
                value = "Far";
            canvas.centered_text(primitive_bounds, value, 1, native_foreground,
                    combined_opacity, 0,
                    native_font ? std::span<const std::byte>{ *native_font }
                                : std::span<const std::byte>{},
                    static_cast<std::uint32_t>(
                            std::max(static_cast<long>(11.0 * scale), 1L)));
            const auto arrow =
                    std::max(static_cast<int>(std::lround(4.0 * scale)), 2);
            const auto arrow_x =
                    primitive_bounds.x + primitive_bounds.width - arrow * 2;
            const auto arrow_y =
                    primitive_bounds.y + primitive_bounds.height / 2;
            for (auto row = 0; row < arrow; ++row)
                canvas.rectangle({ arrow_x + row, arrow_y + row,
                                         std::max(arrow * 2 - row * 2, 1), 1 },
                        native_foreground, combined_opacity * 0.8);
        }

        UiRectangle content = primitive_bounds;
        const auto spacing = static_cast<int>(
                std::lround(number(node.properties, "spacing")
                                    .value_or(raw_kind == "List" ? 2.0 : 0.0) *
                            scale));
        auto stack_alignment = parse_alignment(
                text(node.properties, "alignment").value_or(""));
        if (const auto *align_modifier = modifier(node, "align")) {
            const auto wrapper_alignment = parse_alignment(
                    text(align_modifier->arguments, "#1").value_or(""));
            // an outer alignment moves the stack, it must NOT replace how the
            // stack aligns its children. don't include a later frame either or
            // a top footer gets centered again after its offset
            const auto alignment_index = static_cast<std::size_t>(
                    align_modifier - node.modifiers.data());
            const auto intrinsic = measure(node, parent_kind, alignment_index);
            if (kind == "HStack" && intrinsic.height > 0.0) {
                const auto height = std::min(
                        static_cast<int>(std::lround(intrinsic.height * scale)),
                        content.height);
                if (wrapper_alignment.vertical == "bottom")
                    content.y += content.height - height;
                else if (wrapper_alignment.vertical == "center")
                    content.y += (content.height - height) / 2;
                content.height = height;
            }
            if (kind == "VStack" && intrinsic.width > 0.0 &&
                    !descendant_flexible_axis(node, true)) {
                const auto width = std::min(
                        static_cast<int>(std::lround(intrinsic.width * scale)),
                        content.width);
                if (wrapper_alignment.horizontal == "right")
                    content.x += content.width - width;
                else if (wrapper_alignment.horizontal == "center")
                    content.x += (content.width - width) / 2;
                content.width = width;
            }
        }
        if (kind == "ZStack" && stack_alignment.vertical.empty() &&
                stack_alignment.horizontal.empty())
            stack_alignment = { "center", "center" };
        else if (kind == "HStack" && stack_alignment.vertical.empty())
            stack_alignment.vertical = "center";
        else if (kind == "VStack" && stack_alignment.horizontal.empty())
            stack_alignment.horizontal = "center";
        double cursor_x = content.x;
        double cursor_y = content.y;
        struct StackChild {
            const NativeUiComponentNode *node{};
            Size desired;
            AxisConstraints constraints;
            double allocated{};
        };
        std::vector<StackChild> stack_children;
        auto occupied = 0;
        if (kind == "HStack" || kind == "VStack") {
            std::vector<const NativeUiComponentNode *> layout_children;
            append_layout_children(node, layout_children);
            for (const auto *child : layout_children) {
                auto child_size = measure(*child, kind);
                auto constraints =
                        axis_constraints(*child, kind == "HStack", kind);
                stack_children.push_back({ child, child_size, constraints,
                        constraints.minimum });
            }
            // clang-format off
            const auto available = std::max((kind == "HStack" ? content.width :
                    content.height) / scale - (stack_children.empty() ? 0.0 :
                    (stack_children.size() - 1) * spacing / scale), 0.0);
            // clang-format on
            double remaining = available;
            std::vector<StackChild *> flexible;
            for (auto &child : stack_children) {
                if (tight_constraints(child.constraints))
                    remaining -= child.allocated;
                else
                    flexible.push_back(&child);
            }
            std::ranges::sort(flexible, [](const auto *left,
                                                const auto *right) {
                if (left->constraints.priority != right->constraints.priority)
                    return left->constraints.priority >
                           right->constraints.priority;
                const auto left_unbounded =
                        std::isinf(left->constraints.maximum);
                const auto right_unbounded =
                        std::isinf(right->constraints.maximum);
                if (left_unbounded != right_unbounded)
                    return !left_unbounded;
                if (left->constraints.maximum != right->constraints.maximum)
                    return left->constraints.maximum <
                           right->constraints.maximum;
                return left->constraints.minimum > right->constraints.minimum;
            });
            for (std::size_t index = 0; index < flexible.size(); ++index) {
                const auto group_priority =
                        flexible[index]->constraints.priority;
                auto group_end = index;
                while (group_end < flexible.size() &&
                        flexible[group_end]->constraints.priority ==
                                group_priority)
                    ++group_end;
                for (; index < group_end; ++index) {
                    auto &child = *flexible[index];
                    const auto count = static_cast<double>(group_end - index);
                    const auto proposal = std::max(remaining / count, 0.0);
                    child.allocated =
                            std::clamp(proposal, child.constraints.minimum,
                                    child.constraints.maximum);
                    remaining -= child.allocated;
                }
                --index;
            }
            double occupied_pixels{};
            for (const auto &child : stack_children)
                occupied_pixels += child.allocated * scale;
            occupied = static_cast<int>(std::lround(occupied_pixels));
            if (stack_children.size() > 1)
                occupied +=
                        static_cast<int>(stack_children.size() - 1) * spacing;
        }
        {
            Alignment content_alignment{};
            if (const auto *align_modifier = modifier(node, "align"))
                content_alignment = parse_alignment(
                        text(align_modifier->arguments, "#1").value_or(""));
            if (kind == "HStack" && content_alignment.horizontal == "right")
                cursor_x = content.x + std::max(content.width - occupied, 0);
            else if (kind == "HStack" &&
                     content_alignment.horizontal == "center")
                cursor_x =
                        content.x + std::max((content.width - occupied) / 2, 0);
            if (kind == "VStack" && content_alignment.vertical == "bottom")
                cursor_y = content.y + std::max(content.height - occupied, 0);
            else if (kind == "VStack" && content_alignment.vertical == "center")
                cursor_y = content.y +
                           std::max((content.height - occupied) / 2, 0);
        }
        std::size_t stack_child_index{};
        std::vector<const NativeUiComponentNode *> layout_children;
        append_layout_children(node, layout_children);
        for (const auto *child_pointer : layout_children) {
            const auto &child = *child_pointer;
            auto desired = measure(child, kind);
            auto child_offer = content;
            auto desired_width =
                    static_cast<int>(std::lround(desired.width * scale));
            auto desired_height =
                    static_cast<int>(std::lround(desired.height * scale));
            // the child gets the wrapper's whole proposal, not its own size
            if (kind == "ModifierWrapper") {
                desired_width = content.width;
                desired_height = content.height;
            }
            // still gets the stack's cross axis even in a fixed overlay, the
            // Frame only limits what's measured
            const auto child_layout = layout_kind(child);
            const auto child_is_stack =
                    child_layout == "HStack" || child_layout == "VStack";
            if (kind != "HStack") {
                const auto constraints = axis_constraints(child, true, kind);
                if (std::isfinite(constraints.maximum) &&
                        constraints.maximum > constraints.minimum)
                    desired_width = std::max(desired_width,
                            static_cast<int>(std::lround(
                                    std::min<double>(content.width / scale,
                                            constraints.maximum) *
                                    scale)));
            }
            if (frame_size(child).width <= 0.0 && kind != "HStack" &&
                    (child_is_stack ? (child_layout == "HStack"
                                                      ? descendant_flexible_axis(
                                                                child, true)
                                                      : descendant_alignment_axis(
                                                                child, true) ||
                                                                descendant_flexible_axis(
                                                                        child,
                                                                        true))
                                    : flexible_axis(child, true) ||
                                              descendant_flexible_axis(
                                                      child, true)))
                desired_width = content.width;
            if (frame_size(child).height <= 0.0 && kind != "VStack" &&
                    layout_kind(child) != "HStack" &&
                    (child_is_stack ? (child_layout == "VStack"
                                                      ? descendant_flexible_axis(
                                                                child, false)
                                                      : descendant_alignment_axis(
                                                                child, false))
                                    : flexible_axis(child, false) ||
                                              descendant_flexible_axis(
                                                      child, false)))
                desired_height = content.height;
            // the square comes first, so a width-only frame gives back the
            // ring's height and not what it was offered

            if (short_kind(child) == "Ring") {
                const auto width = frame_limit(child, "width");
                const auto height = frame_limit(child, "height");
                // clang-format off
                const auto minimum = (number(child.properties,
                        "thickness").value_or(4.0) * 2.0 + 6.0) * scale;
                // clang-format on
                if (width && !height)
                    desired_height = static_cast<int>(std::lround(std::max(
                            std::min(*width * scale, double(desired_height)),
                            minimum)));
                if (height && !width)
                    desired_width = static_cast<int>(std::lround(std::max(
                            std::min(*height * scale, double(desired_width)),
                            minimum)));
            }
            double allocated_pixels =
                    kind == "HStack" ? desired_width : desired_height;
            if ((kind == "HStack" || kind == "VStack") &&
                    stack_child_index < stack_children.size()) {
                allocated_pixels =
                        stack_children[stack_child_index++].allocated * scale;
                const auto allocated =
                        static_cast<int>(std::lround(allocated_pixels));
                if (kind == "HStack")
                    desired_width = allocated;
                else
                    desired_height = allocated;
            }
            // alignment inside a frame positions its content, not the frame;
            // the linear stack still has to position that outer item
            bool child_has_alignment = false;
            for (const auto &value : child.modifiers) {
                if (value.name == "frame")
                    child_has_alignment = false;
                else if (value.name == "align")
                    child_has_alignment = true;
            }
            if (kind == "HStack") {
                child_offer.x = static_cast<int>(std::lround(cursor_x));
                child_offer.width = static_cast<int>(std::lround(
                                            cursor_x + allocated_pixels)) -
                                    child_offer.x;
                cursor_x += allocated_pixels + spacing;
                if (!child_has_alignment &&
                        (desired_height > 0 || child_layout == "Spacer")) {
                    if (stack_alignment.vertical == "bottom")
                        child_offer.y += child_offer.height - desired_height;
                    else if (stack_alignment.vertical == "center")
                        child_offer.y +=
                                (child_offer.height - desired_height) / 2;
                    child_offer.height = desired_height;
                }
            } else if (kind == "VStack") {
                child_offer.y = static_cast<int>(std::lround(cursor_y));
                child_offer.height = static_cast<int>(std::lround(
                                             cursor_y + allocated_pixels)) -
                                     child_offer.y;
                cursor_y += allocated_pixels + spacing;
                if (!child_has_alignment &&
                        (desired_width > 0 || child_layout == "Spacer")) {
                    if (stack_alignment.horizontal == "right")
                        child_offer.x += child_offer.width - desired_width;
                    else if (stack_alignment.horizontal == "center")
                        child_offer.x +=
                                (child_offer.width - desired_width) / 2;
                    child_offer.width = desired_width;
                }
            } else if (kind == "ZStack" && !child_has_alignment &&
                       modifier(child, "position") == nullptr) {
                if (desired_width > 0) {
                    if (stack_alignment.horizontal == "right")
                        child_offer.x += child_offer.width - desired_width;
                    else if (stack_alignment.horizontal == "center")
                        child_offer.x +=
                                (child_offer.width - desired_width) / 2;
                    child_offer.width = desired_width;
                }
                if (desired_height > 0) {
                    if (stack_alignment.vertical == "bottom")
                        child_offer.y += child_offer.height - desired_height;
                    else if (stack_alignment.vertical == "center")
                        child_offer.y +=
                                (child_offer.height - desired_height) / 2;
                    child_offer.height = desired_height;
                }
            }
            draw(child, child_offer, combined_opacity, native_foreground, kind,
                    enabled);
        }
        for (const auto &child : node.children)
            if (child.role.starts_with("modifier:overlay:"))
                draw(child, modifier_offer(child, "overlay"), combined_opacity,
                        native_foreground, {}, enabled);
        for (const auto &child : node.children)
            if (child.role.starts_with("modifier:foreground:"))
                draw(child, modifier_offer(child, "foreground"),
                        combined_opacity, native_foreground, {}, enabled);
        for (std::size_t i = 0; i < node.modifiers.size(); ++i) {
            const auto &popup = node.modifiers[i];
            if (popup.name != "popover" || !flag(popup.arguments, "visible"))
                continue;
            const auto role =
                    "modifier:popover:" + std::to_string(i + 1) + ":content";
            for (const auto &child : node.children)
                if (child.role == role)
                    popovers.push_back({ &child, &popup,
                            unwrap_layout_modifiers(
                                    node, bounds, parent_kind, i + 1),
                            combined_opacity, native_foreground, enabled });
        }
    }

    const UiDocument &document;
    const UiAssetProvider *assets{};
    NativeCanvas canvas;
    float scale{};
    std::shared_ptr<const std::vector<std::byte>> native_font;
    std::unordered_map<const NativeUiComponentNode *,
            std::shared_ptr<const std::vector<std::byte>>>
            node_fonts;
    UiRectangle root{};
    std::vector<NativeUiHitRegion> *hits{};
    std::vector<Popover> popovers;
    mutable std::unordered_map<MeasureKey, Size, MeasureKeyHash> measure_cache;
};

} // namespace

void render_native_ui_components(const UiDocument &document,
        const UiAssetProvider *assets, UiFrame &frame, const float scale,
        std::vector<NativeUiHitRegion> *hits) {
    if (!assets || document.native_ui_component_roots.empty())
        return;
    profile_ring_us = 0;
    profile_ring_count = 0;
    const auto profile_start = std::chrono::steady_clock::now();
    NativeLayout(document, assets, frame, scale, hits).render();
    if (std::getenv("EKPHORE_PROFILE_CIRCULAR_DRAG"))
        std::fprintf(stderr, "native-layout-us=%lld ring=%lld/%zu\n",
                static_cast<long long>(
                        std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now() -
                                profile_start)
                                .count()),
                static_cast<long long>(profile_ring_us), profile_ring_count);
}

} // namespace ekphore
