// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../ui/browser.h"
#include "audio_file.h"

#include <libnki/libnki.h>

#include <algorithm>
#include <cctype>
#include <exception>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <string_view>

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#endif

namespace ekphore::sampling {

libnki::ResolvedPathInfo resolve_sample_path(const libnki::SampleInfo &sample,
        const SamplePathResolutionContext &context) {
    auto paths = context.paths;
    const auto mode = sample.content_file_path_mode.value_or(0);
    if (mode == 1 || mode == 2) {
        // pick the root first, a missing file doesn't mean we try another
        std::optional<std::filesystem::path> root;
        if (mode == 1) {
            if (sample.content_file_library_id && context.library_root)
                root = context.library_root(*sample.content_file_library_id);
            if (!root) {
                root = context.default_sample_base;
                if (!root || root->empty())
                    root = context.user_documents;
            }
        } else {
            root = context.shared_application_data;
        }
        if (!root || root->empty()) {
            libnki::ResolvedPathInfo missing;
            missing.status = libnki::PathResolutionStatus::missing_context_root;
            return missing;
        }
        // relative, so another marker must NOT override the root
        if (sample.serialized_path &&
                !sample.serialized_path->segments.empty()) {
            const auto first = sample.serialized_path->segments.front().kind;
            if (first != 2 && first != 3 && first != 4 && first != 8 &&
                    first != 9) {
                libnki::ResolvedPathInfo unsupported;
                unsupported.status =
                        libnki::PathResolutionStatus::unsupported_segment;
                return unsupported;
            }
        }
        paths.read_context = root;
    }
    if (sample.serialized_path)
        return libnki::resolve_path(*sample.serialized_path, paths);
    libnki::ResolvedPathInfo resolved;
    if (!sample.path || sample.path->empty())
        return resolved;
    auto path = std::filesystem::path(*sample.path);
    if (path.is_relative()) {
        if (!paths.read_context) {
            resolved.status =
                    libnki::PathResolutionStatus::missing_context_root;
            return resolved;
        }
        path = *paths.read_context / path;
    }
    resolved.status = libnki::PathResolutionStatus::resolved;
    resolved.path = path.lexically_normal();
    return resolved;
}

std::optional<std::filesystem::path>
resolve_registered_library_root_with_install_state(
        const std::uint32_t library_id,
        const libnki::NicntProductHints &products,
        const LibraryRegistrationReader &reader,
        const LibraryPathConverter &converter,
        const std::function<std::optional<std::filesystem::path>(
                const std::string &)> &custom) {
    const libnki::NicntProductInfo *product = nullptr;
    for (const auto &candidate : products.products) {
        if (std::find(candidate.snpid_values.begin(),
                    candidate.snpid_values.end(),
                    library_id) == candidate.snpid_values.end())
            continue;
        // two matches, we'd need the whole product database to pick one
        if (product)
            return std::nullopt;
        product = &candidate;
    }
    if (!product)
        return std::nullopt;
    std::optional<std::string> key;
    for (const auto &candidate : product->reg_keys) {
        if (candidate.target_info.platform_value != 0 &&
                candidate.target_info.platform_value != 2)
            continue;
        const auto &target = candidate.target_info;
        // we can't pick by app/version here, don't just take the first one
        if (target.target_mask != 0xfff || target.versions.min_version ||
                target.versions.max_version || target.min_os_version_mac ||
                target.max_os_version_mac || target.min_os_version_native_os ||
                target.max_os_version_native_os || target.legacy_os_min ||
                target.legacy_os_max)
            return std::nullopt;
        if (candidate.value.empty())
            continue;
        if (key && *key != candidate.value)
            return std::nullopt;
        key = candidate.value;
    }
    if (!key)
        return std::nullopt;
    // custom wins, but empty or "/" isn't a real directory
    if (custom) {
        const auto root = custom(*key);
        if (root && !root->empty() && *root != "/")
            return root;
    }
    // the NI prefixed one only if the normal one isn't there
    auto registration = reader(*key);
    if (!registration)
        registration = reader("com.native-instruments." + *key);
    if (!registration)
        return std::nullopt;
    const auto value = registration->content_directory
                               ? registration->content_directory
                               : registration->system_content_directory;
    if (!value || value->empty())
        return std::nullopt;
    return converter(*value);
}

std::optional<std::filesystem::path> resolve_registered_library_root(
        const std::uint32_t library_id,
        const libnki::NicntProductHints &products,
        const LibraryRegistrationReader &reader,
        const LibraryPathConverter &converter) {
    return resolve_registered_library_root_with_install_state(
            library_id, products, reader, converter, {});
}

namespace {
// check the whole thing even though we only want ContentDir, half a document
// doesn't count
class InstallationJson {
    std::string_view text;
    std::size_t cursor{};
    void whitespace() {
        while (cursor < text.size() &&
                (text[cursor] == ' ' || text[cursor] == '\n' ||
                        text[cursor] == '\r' || text[cursor] == '\t'))
            ++cursor;
    }
    bool take(char c) {
        whitespace();
        if (cursor == text.size() || text[cursor] != c)
            return false;
        ++cursor;
        return true;
    }
    unsigned hex4() {
        unsigned value = 0;
        for (int i = 0; i < 4; ++i) {
            if (cursor == text.size())
                throw std::runtime_error("short JSON escape");
            const char c = text[cursor++];
            const int digit = c >= '0' && c <= '9'   ? c - '0'
                              : c >= 'a' && c <= 'f' ? c - 'a' + 10
                              : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                                     : -1;
            if (digit < 0)
                throw std::runtime_error("invalid JSON escape");
            value = value * 16 + static_cast<unsigned>(digit);
        }
        return value;
    }
    std::string string() {
        if (!take('"'))
            throw std::runtime_error("expected JSON string");
        std::string result;
        while (cursor < text.size()) {
            const unsigned char c = text[cursor++];
            if (c == '"')
                return result;
            if (c < 32)
                throw std::runtime_error("JSON control character");
            if (c != '\\') {
                result += static_cast<char>(c);
                continue;
            }
            if (cursor == text.size())
                break;
            switch (text[cursor++]) {
                case '"':
                    result += '"';
                    break;
                case '\\':
                    result += '\\';
                    break;
                case '/':
                    result += '/';
                    break;
                case 'b':
                    result += '\b';
                    break;
                case 'f':
                    result += '\f';
                    break;
                case 'n':
                    result += '\n';
                    break;
                case 'r':
                    result += '\r';
                    break;
                case 't':
                    result += '\t';
                    break;
                case 'u': {
                    unsigned u = hex4();
                    if (u >= 0xd800 && u <= 0xdbff) {
                        if (cursor + 2 > text.size() ||
                                text.substr(cursor, 2) != "\\u")
                            throw std::runtime_error(
                                    "missing JSON low surrogate");
                        cursor += 2;
                        const unsigned low = hex4();
                        if (low < 0xdc00 || low > 0xdfff)
                            throw std::runtime_error("invalid JSON surrogate");
                        u = 0x10000 + (u - 0xd800) * 1024 + low - 0xdc00;
                    } else if (u >= 0xdc00 && u <= 0xdfff) {
                        throw std::runtime_error("unpaired JSON surrogate");
                    }
                    if (u < 0x80)
                        result += static_cast<char>(u);
                    else {
                        if (u >= 0x10000)
                            result += static_cast<char>(0xf0 | (u >> 18));
                        if (u >= 0x800)
                            result += static_cast<char>(
                                    (u >= 0x10000 ? 0x80 : 0xe0) |
                                    ((u >> 12) & 63));
                        result += static_cast<char>(
                                (u >= 0x800 ? 0x80 : 0xc0) | ((u >> 6) & 63));
                        result += static_cast<char>(0x80 | (u & 63));
                    }
                    break;
                }
                default:
                    throw std::runtime_error("invalid JSON escape");
            }
        }
        throw std::runtime_error("unterminated JSON string");
    }
    void value(unsigned depth) {
        if (depth > 64)
            throw std::runtime_error("JSON nesting limit");
        whitespace();
        if (cursor == text.size())
            throw std::runtime_error("missing JSON value");
        if (text[cursor] == '"') {
            (void)string();
            return;
        }
        if (take('{')) {
            if (take('}'))
                return;
            do {
                (void)string();
                if (!take(':'))
                    throw std::runtime_error("missing JSON colon");
                value(depth + 1);
            } while (take(','));
            if (!take('}'))
                throw std::runtime_error("missing JSON object end");
            return;
        }
        if (take('[')) {
            if (take(']'))
                return;
            do {
                value(depth + 1);
            } while (take(','));
            if (!take(']'))
                throw std::runtime_error("missing JSON array end");
            return;
        }
        for (const auto literal : { "true", "false", "null" }) {
            const std::string_view word(literal);
            if (text.substr(cursor, word.size()) == word) {
                cursor += word.size();
                return;
            }
        }
        if (text[cursor] == '-')
            ++cursor;
        const auto digits = [&] {
            const auto begin = cursor;
            while (cursor < text.size() && text[cursor] >= '0' &&
                    text[cursor] <= '9')
                ++cursor;
            if (begin == cursor)
                throw std::runtime_error("invalid JSON number");
        };
        if (cursor < text.size() && text[cursor] == '0')
            ++cursor;
        else
            digits();
        if (cursor < text.size() && text[cursor] == '.') {
            ++cursor;
            digits();
        }
        if (cursor < text.size() &&
                (text[cursor] == 'e' || text[cursor] == 'E')) {
            ++cursor;
            if (cursor < text.size() &&
                    (text[cursor] == '+' || text[cursor] == '-'))
                ++cursor;
            digits();
        }
    }

  public:
    explicit InstallationJson(std::string_view input) : text(input) {}
    std::optional<std::string> content_directory() {
        if (!take('{'))
            throw std::runtime_error("expected installation JSON object");
        std::optional<std::string> root;
        if (!take('}')) {
            do {
                const auto key = string();
                if (!take(':'))
                    throw std::runtime_error("missing JSON colon");
                if (key == "ContentDir")
                    root = string(); // null and numbers aren't valid paths
                else
                    value(1);
            } while (take(','));
            if (!take('}'))
                throw std::runtime_error("missing installation JSON end");
        }
        whitespace();
        if (cursor != text.size())
            throw std::runtime_error("trailing installation JSON");
        return root;
    }
};

#ifdef __APPLE__
std::optional<std::string> utf8(CFStringRef value) {
    if (!value)
        return std::nullopt;
    const auto capacity =
            CFStringGetMaximumSizeForEncoding(
                    CFStringGetLength(value), kCFStringEncodingUTF8) +
            1;
    std::string result(static_cast<std::size_t>(capacity), '\0');
    if (!CFStringGetCString(
                value, result.data(), capacity, kCFStringEncodingUTF8))
        return std::nullopt;
    result.resize(std::char_traits<char>::length(result.c_str()));
    return result;
}

std::optional<std::filesystem::path> macos_directory(
        unsigned long kind, unsigned long domains) {
    // dlopen Foundation, we just want the first directory
    static void *foundation =
            dlopen("/System/Library/Frameworks/Foundation.framework/Foundation",
                    RTLD_LAZY | RTLD_LOCAL);
    if (!foundation)
        return std::nullopt;
    using Search = CFArrayRef (*)(unsigned long, unsigned long, bool);
    static const auto search = reinterpret_cast<Search>(
            dlsym(foundation, "NSSearchPathForDirectoriesInDomains"));
    static const auto push = reinterpret_cast<void *(*)()>(
            dlsym(RTLD_DEFAULT, "objc_autoreleasePoolPush"));
    static const auto pop = reinterpret_cast<void (*)(void *)>(
            dlsym(RTLD_DEFAULT, "objc_autoreleasePoolPop"));
    if (!search || !push || !pop)
        return std::nullopt;
    void *pool = push();
    const auto directories = search(kind, domains, true);
    std::optional<std::string> directory;
    if (directories && CFArrayGetCount(directories) > 0) {
        directory = utf8(static_cast<CFStringRef>(
                CFArrayGetValueAtIndex(directories, 0)));
    }
    pop(pool);
    if (!directory || directory->empty())
        return std::nullopt;
    return std::filesystem::path(*directory);
}

#endif
} // namespace

std::optional<std::filesystem::path> read_custom_installation_root(
        const std::filesystem::path &file,
        const std::optional<std::filesystem::file_time_type> &registry_time) {
    std::error_code error;
    const auto time = std::filesystem::last_write_time(file, error);
    // whole seconds, subsecond differences don't make it stale
    if (error ||
            (registry_time && std::chrono::floor<std::chrono::seconds>(time) <
                                      std::chrono::floor<std::chrono::seconds>(
                                              *registry_time)))
        return std::nullopt;
    const auto size = std::filesystem::file_size(file, error);
    if (error || size > 1024 * 1024)
        return std::nullopt; // too big
    std::ifstream input(file, std::ios::binary);
    if (!input)
        return std::nullopt;
    std::string json(static_cast<std::size_t>(size), '\0');
    input.read(json.data(), static_cast<std::streamsize>(json.size()));
    if (input.gcount() != static_cast<std::streamsize>(json.size()) ||
            input.peek() != std::char_traits<char>::eof())
        return std::nullopt;
    try {
        const auto root = InstallationJson(json).content_directory();
        // has to be POSIX already, we don't convert HFS here
        if (!root || root->empty() || root->front() != '/' ||
                root->find('\0') != std::string::npos)
            return std::nullopt;
        return std::filesystem::path(*root);
    } catch (const std::exception &) { return std::nullopt; }
}

std::optional<std::filesystem::path> convert_registered_library_path(
        const std::string &value) {
    if (value.empty())
        return std::nullopt;
    if (value.starts_with('/'))
        return std::filesystem::path(value);
#ifdef __APPLE__
    // let CoreFoundation deal with HFS paths
    const auto text = CFStringCreateWithCString(
            nullptr, value.c_str(), kCFStringEncodingUTF8);
    if (!text)
        return std::nullopt;
    const auto url = CFURLCreateWithFileSystemPath(
            nullptr, text, kCFURLHFSPathStyle, true);
    CFRelease(text);
    if (!url)
        return std::nullopt;
    const auto path = CFURLCopyFileSystemPath(url, kCFURLPOSIXPathStyle);
    CFRelease(url);
    const auto converted = utf8(path);
    if (path)
        CFRelease(path);
    if (!converted || converted->empty())
        return std::nullopt;
    return std::filesystem::path(*converted);
#else
    return std::nullopt;
#endif
}

SamplePathResolutionContext make_sample_path_resolution_context(
        const std::filesystem::path &instrument_path) {
    SamplePathResolutionContext context;
    context.paths.read_context = instrument_path.parent_path();
    auto libraries =
            std::make_shared<const std::vector<libnki::LocalLibraryInfo>>(
                    libnki::discover_local_libraries(instrument_path));
    context.library_root =
            [libraries](
                    std::uint32_t id) -> std::optional<std::filesystem::path> {
        std::optional<std::filesystem::path> root;
        for (const auto &library : *libraries)
            for (const auto &product : library.metadata.products)
                if (std::ranges::find(product.snpid_values, id) !=
                        product.snpid_values.end()) {
                    if (root)
                        return std::nullopt; // more than one, can't pick
                    root = library.root;
                }
        return root;
    };
    context.resource_key =
            [libraries, private_key = keys::private_key(instrument_path)](
                    std::uint32_t id) {
                return libnki::derive_local_product_encryption_key(
                        private_key, *libraries, id);
            };
#ifdef __APPLE__

    // kontakt 8's, NOT another version just because the folder is there
    static const auto factory = []() -> std::optional<std::filesystem::path> {
        const auto directory = macos_directory(14, 8);
        if (!directory)
            return std::nullopt;
        return *directory / "Native Instruments" / "Kontakt 8";
    }();
    context.paths.factory_directory = factory;
    context.shared_application_data = factory;
#endif
    // don't guess the others
    return context;
}

libnki::ResolvedPathInfo resolve_sample_path(const libnki::SampleInfo &sample,
        const std::filesystem::path &instrument_path) {
    return resolve_sample_path(
            sample, make_sample_path_resolution_context(instrument_path));
}
} // namespace ekphore::sampling
