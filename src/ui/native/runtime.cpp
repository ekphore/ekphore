// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime.h"
#include "../trace.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <set>
#include <utility>

namespace ekphore {

namespace {

bool identifier_start(const char value) {
    return std::isalpha(static_cast<unsigned char>(value)) || value == '_';
}

bool identifier_continue(const char value) {
    return std::isalnum(static_cast<unsigned char>(value)) || value == '_';
}

void skip_line(std::string_view source, std::size_t &cursor) {
    const auto newline = source.find('\n', cursor);
    cursor = newline == std::string_view::npos ? source.size() : newline + 1;
}

bool skip_long_bracket(std::string_view source, std::size_t &cursor) {
    if (cursor >= source.size() || source[cursor] != '[')
        return false;
    std::size_t equals = cursor + 1;
    while (equals < source.size() && source[equals] == '=')
        ++equals;
    if (equals >= source.size() || source[equals] != '[')
        return false;
    const auto marker_length = equals - cursor - 1;
    cursor = equals + 1;
    while (cursor < source.size()) {
        if (source[cursor] != ']') {
            ++cursor;
            continue;
        }
        std::size_t close = cursor + 1;
        std::size_t matched{};
        while (close < source.size() && source[close] == '=' &&
                matched < marker_length) {
            ++close;
            ++matched;
        }
        if (matched == marker_length && close < source.size() &&
                source[close] == ']') {
            cursor = close + 1;
            return true;
        }
        ++cursor;
    }
    return true;
}

void skip_space_and_comments(std::string_view source, std::size_t &cursor) {
    for (;;) {
        while (cursor < source.size() &&
                std::isspace(static_cast<unsigned char>(source[cursor])))
            ++cursor;
        if (cursor + 1 >= source.size() || source[cursor] != '-' ||
                source[cursor + 1] != '-')
            return;
        cursor += 2;
        if (cursor < source.size() && source[cursor] == '[') {
            const auto before = cursor;
            if (skip_long_bracket(source, cursor))
                continue;
            cursor = before;
        }
        skip_line(source, cursor);
    }
}

std::optional<std::string> quoted_string(
        std::string_view source, std::size_t &cursor) {
    if (cursor >= source.size() ||
            (source[cursor] != '\'' && source[cursor] != '"'))
        return std::nullopt;
    const char quote = source[cursor++];
    std::string result;
    while (cursor < source.size()) {
        const char value = source[cursor++];
        if (value == quote)
            return result;
        if (value != '\\' || cursor >= source.size()) {
            result.push_back(value);
            continue;
        }
        const char escaped = source[cursor++];
        switch (escaped) {
            case 'n':
                result.push_back('\n');
                break;
            case 'r':
                result.push_back('\r');
                break;
            case 't':
                result.push_back('\t');
                break;
            default:
                result.push_back(escaped);
                break;
        }
    }
    return std::nullopt;
}

std::string canonical_module(std::string_view module) {
    while (module.starts_with("./"))
        module.remove_prefix(2);
    if (module.ends_with(".nui"))
        module.remove_suffix(4);
    return std::string(module);
}

void append_unique(std::vector<std::string> &values, std::string value) {
    if (!value.empty() && std::ranges::find(values, value) == values.end())
        values.push_back(std::move(value));
}

std::optional<std::string> identifier_path(
        std::string_view source, std::size_t &cursor) {
    if (cursor >= source.size() || !identifier_start(source[cursor]))
        return std::nullopt;
    std::string result;
    for (;;) {
        const auto begin = cursor++;
        while (cursor < source.size() && identifier_continue(source[cursor]))
            ++cursor;
        if (!result.empty())
            result.push_back('.');
        result.append(source.substr(begin, cursor - begin));
        if (cursor >= source.size() || source[cursor] != '.' ||
                cursor + 1 >= source.size() ||
                !identifier_start(source[cursor + 1]))
            break;
        ++cursor;
    }
    return result;
}

enum class VisitState { visiting, complete };

class Resolver {
  public:
    explicit Resolver(const NativeUiModuleSource &source)
        : input_source(source) {}

    NativeUiModuleGraph run(std::string_view root) {
        root_module = canonical_module(root);
        visit(root_module);
        std::ranges::sort(graph.external_modules);
        graph.external_modules.erase(std::unique(graph.external_modules.begin(),
                                             graph.external_modules.end()),
                graph.external_modules.end());
        return std::move(graph);
    }

  private:
    void visit(const std::string &module) {
        if (const auto found = states.find(module); found != states.end()) {
            if (found->second == VisitState::visiting)
                graph.diagnostics.push_back(
                        "recursive native UI import: " + module);
            return;
        }
        states[module] = VisitState::visiting;
        const auto loaded = input_source ? input_source(module) : std::nullopt;
        if (!loaded) {
            if (module == root_module)
                graph.diagnostics.push_back(
                        "native UI root module not found: " + module);
            else
                graph.external_modules.push_back(module);
            states[module] = VisitState::complete;
            return;
        }
        auto imports = native_ui_imports(*loaded);
        for (auto &imported : imports)
            imported = canonical_module(imported);
        for (const auto &imported : imports)
            visit(imported);
        graph.modules.push_back({
                module,
                *loaded,
                std::move(imports),
                native_ui_declarative_components(*loaded),
                native_ui_method_calls(*loaded),
        });
        states[module] = VisitState::complete;
    }

    const NativeUiModuleSource &input_source;
    std::string root_module;
    std::map<std::string, VisitState, std::less<>> states;
    NativeUiModuleGraph graph;
};

class DeclarativeLowerer {
  public:
    explicit DeclarativeLowerer(const std::string_view source)
        : input_source(source) {}

    NativeUiLowering run() {
        return { lower_range(0, input_source.size()), std::move(diagnostics) };
    }

  private:
    static bool entry_follows(const std::string_view text, std::size_t cursor) {
        while (cursor < text.size() &&
                std::isspace(static_cast<unsigned char>(text[cursor])))
            ++cursor;
        if (cursor >= text.size())
            return false;
        if (text.substr(cursor).starts_with("__nui_component(") ||
                text.substr(cursor).starts_with("__nui_modifier("))
            return true;
        if (text[cursor] == '(')
            return true;
        if (!identifier_start(text[cursor]))
            return false;
        ++cursor;
        while (cursor < text.size() && identifier_continue(text[cursor]))
            ++cursor;
        while (cursor < text.size() &&
                (text[cursor] == ' ' || text[cursor] == '\t'))
            ++cursor;
        return cursor < text.size() && text[cursor] == '=';
    }

    static std::string comma_separate_body(const std::string_view body) {
        std::string output;
        int delimiters{};
        int function_depth{};
        std::size_t cursor{};
        char previous_significant{};
        while (cursor < body.size()) {
            if (body[cursor] == '\'' || body[cursor] == '"') {
                const auto start = cursor;
                (void)quoted_string(body, cursor);
                output.append(body.substr(start, cursor - start));
                previous_significant = 's';
                continue;
            }
            if (body[cursor] == '[') {
                const auto start = cursor;
                if (skip_long_bracket(body, cursor)) {
                    output.append(body.substr(start, cursor - start));
                    previous_significant = 's';
                    continue;
                }
            }
            if (cursor + 1 < body.size() && body[cursor] == '-' &&
                    body[cursor + 1] == '-') {
                const auto start = cursor;
                cursor += 2;
                if (cursor >= body.size() || body[cursor] != '[' ||
                        !skip_long_bracket(body, cursor))
                    skip_line(body, cursor);
                if (delimiters == 0 && function_depth == 0 &&
                        previous_significant != ',' &&
                        previous_significant != 0 &&
                        previous_significant != '=' &&
                        entry_follows(body, cursor)) {
                    output.push_back(',');
                    previous_significant = ',';
                }
                output.append(body.substr(start, cursor - start));
                continue;
            }
            if (identifier_start(body[cursor])) {
                const auto start = cursor++;
                while (cursor < body.size() &&
                        identifier_continue(body[cursor]))
                    ++cursor;
                const auto token = body.substr(start, cursor - start);
                if (delimiters == 0 && function_depth == 0 &&
                        entry_follows(body, start) &&
                        (previous_significant == 's' ||
                                previous_significant == 'i' ||
                                previous_significant == ')' ||
                                previous_significant == ']' ||
                                previous_significant == '}' ||
                                std::isdigit(static_cast<unsigned char>(
                                        previous_significant)))) {
                    output.push_back(',');
                    previous_significant = ',';
                }
                if (token == "function" || token == "if" || token == "for" ||
                        token == "while" || token == "repeat")
                    ++function_depth;
                else if ((token == "end" || token == "until") &&
                         function_depth > 0)
                    --function_depth;
                output.append(token);
                previous_significant =
                        token == "and" || token == "or" || token == "not" ? '='
                                                                          : 'i';
                continue;
            }
            const char value = body[cursor++];
            if (value == '(' || value == '[' || value == '{')
                ++delimiters;
            if ((value == ')' || value == ']' || value == '}') &&
                    delimiters > 0)
                --delimiters;
            if (value == '\n' && delimiters == 0 && function_depth == 0 &&
                    previous_significant != ',' && previous_significant != 0 &&
                    previous_significant != '=' &&
                    entry_follows(body, cursor)) {
                output.push_back(',');
                previous_significant = ',';
            }
            output.push_back(value);
            if (!std::isspace(static_cast<unsigned char>(value)))
                previous_significant = value;
        }
        return output;
    }

    std::size_t matching_parenthesis(
            const std::size_t open, const std::size_t end) {
        std::size_t cursor = open + 1;
        unsigned depth = 1;
        while (cursor < end) {
            if (input_source[cursor] == '\'' || input_source[cursor] == '"') {
                (void)quoted_string(input_source, cursor);
                continue;
            }
            if (input_source[cursor] == '[' &&
                    skip_long_bracket(input_source, cursor))
                continue;
            if (cursor + 1 < end && input_source[cursor] == '-' &&
                    input_source[cursor + 1] == '-') {
                cursor += 2;
                if (cursor >= end || input_source[cursor] != '[' ||
                        !skip_long_bracket(input_source, cursor))
                    skip_line(input_source, cursor);
                continue;
            }
            if (input_source[cursor] == '(')
                ++depth;
            if (input_source[cursor] == ')' && --depth == 0)
                return cursor;
            ++cursor;
        }
        return std::string_view::npos;
    }

    std::size_t matching_brace(const std::size_t open) {
        std::size_t cursor = open + 1;
        unsigned depth = 1;
        while (cursor < input_source.size()) {
            if (input_source[cursor] == '\'' || input_source[cursor] == '"') {
                (void)quoted_string(input_source, cursor);
                continue;
            }
            if (input_source[cursor] == '[' &&
                    skip_long_bracket(input_source, cursor))
                continue;
            if (cursor + 1 < input_source.size() &&
                    input_source[cursor] == '-' &&
                    input_source[cursor + 1] == '-') {
                cursor += 2;
                if (cursor >= input_source.size() ||
                        input_source[cursor] != '[' ||
                        !skip_long_bracket(input_source, cursor))
                    skip_line(input_source, cursor);
                continue;
            }
            if (input_source[cursor] == '{')
                ++depth;
            if (input_source[cursor] == '}' && --depth == 0)
                return cursor;
            ++cursor;
        }
        return std::string_view::npos;
    }

    std::string lower_range(const std::size_t begin, const std::size_t end) {
        std::string output;
        std::size_t cursor = begin;
        while (cursor < end) {
            if (input_source[cursor] == '\'' || input_source[cursor] == '"') {
                const auto start = cursor;
                (void)quoted_string(input_source, cursor);
                output.append(input_source.substr(start, cursor - start));
                continue;
            }
            if (input_source[cursor] == '[') {
                const auto start = cursor;
                if (skip_long_bracket(input_source, cursor)) {
                    output.append(input_source.substr(start, cursor - start));
                    continue;
                }
            }
            if (cursor + 1 < end && input_source[cursor] == '-' &&
                    input_source[cursor + 1] == '-') {
                const auto start = cursor;
                cursor += 2;
                if (cursor >= end || input_source[cursor] != '[' ||
                        !skip_long_bracket(input_source, cursor))
                    skip_line(input_source, cursor);
                output.append(input_source.substr(start, cursor - start));
                continue;
            }
            if (input_source[cursor] != '@') {
                output.push_back(input_source[cursor++]);
                continue;
            }

            const auto marker = cursor++;
            while (cursor < end && std::isspace(static_cast<unsigned char>(
                                           input_source[cursor])))
                ++cursor;
            std::string component;
            if (cursor < end && input_source[cursor] != '{') {
                auto parsed = identifier_path(input_source, cursor);
                if (!parsed) {
                    output.push_back('@');
                    cursor = marker + 1;
                    continue;
                }
                component = std::move(*parsed);
                while (cursor < end && std::isspace(static_cast<unsigned char>(
                                               input_source[cursor])))
                    ++cursor;
            }
            if (cursor >= end || input_source[cursor] != '{') {
                diagnostics.push_back(
                        "native UI declarative marker without body at byte " +
                        std::to_string(marker));
                output.append(input_source.substr(marker, cursor - marker));
                continue;
            }
            const auto close = matching_brace(cursor);
            if (close == std::string_view::npos || close >= end) {
                diagnostics.push_back(
                        "unterminated native UI declaration at byte " +
                        std::to_string(marker));
                output.append(input_source.substr(marker, end - marker));
                break;
            }
            const auto lowered_body = lower_range(cursor + 1, close);
            const auto body = component.empty()
                                      ? lowered_body
                                      : comma_separate_body(lowered_body);
            std::string expression;
            if (component.empty()) {
                expression.push_back('(');
                expression.append(body);
                expression.push_back(')');
            } else {
                expression.append("__nui_component(");
                expression.append(component);
                expression.append(", {");
                expression.append(body);
                expression.append("})");
            }
            cursor = close + 1;
            for (;;) {
                auto chain = cursor;
                for (;;) {
                    while (chain < end &&
                            std::isspace(static_cast<unsigned char>(
                                    input_source[chain])))
                        ++chain;
                    if (chain + 1 >= end || input_source[chain] != '-' ||
                            input_source[chain + 1] != '-')
                        break;
                    chain += 2;
                    if (chain < end && input_source[chain] == '[') {
                        if (skip_long_bracket(input_source, chain))
                            continue;
                    }
                    skip_line(input_source, chain);
                }
                if (chain >= end || input_source[chain] != '.')
                    break;
                ++chain;
                auto method = identifier_path(input_source, chain);
                if (!method)
                    break;
                while (chain < end && std::isspace(static_cast<unsigned char>(
                                              input_source[chain])))
                    ++chain;
                if (chain >= end || input_source[chain] != '(')
                    break;
                const auto method_close = matching_parenthesis(chain, end);
                if (method_close == std::string_view::npos) {
                    diagnostics.push_back(
                            "unterminated native UI modifier at byte " +
                            std::to_string(chain));
                    break;
                }
                const auto arguments = lower_range(chain + 1, method_close);
                expression = "__nui_modifier(" + expression + ", \"" + *method +
                             "\", {" + arguments + "})";
                cursor = method_close + 1;
            }
            output.append(expression);
        }
        return output;
    }

    std::string_view input_source;
    std::vector<std::string> diagnostics;
};

thread_local std::int64_t profile_factory_us{};
thread_local std::int64_t profile_compact_us{};
thread_local std::int64_t profile_foreach_us{};
thread_local std::int64_t profile_factory_count{};
thread_local std::int64_t profile_compact_count{};

void push_lua_proxy(lua_State *state, const std::string_view name);

int lua_proxy_identity(lua_State *state) {
    lua_getfield(state, 1, "__nui_name");
    const char *raw_name = lua_tostring(state, -1);
    const std::string_view name = raw_name ? raw_name : "";
    lua_pop(state, 1);
    if (name.ends_with(".util.generate_range")) {
        const auto count = std::max<lua_Integer>(lua_tointeger(state, 2), 0);
        lua_newtable(state);
        for (lua_Integer index = 1; index <= count; ++index) {
            lua_pushinteger(state, index);
            lua_rawseti(state, -2, index);
        }
        return 1;
    }
    if (name.ends_with(".util.map_range") && lua_gettop(state) >= 6) {
        const auto value = lua_tonumber(state, 2);
        const auto from_min = lua_tonumber(state, 3);
        const auto from_max = lua_tonumber(state, 4);
        const auto to_min = lua_tonumber(state, 5);
        const auto to_max = lua_tonumber(state, 6);
        const auto span = from_max - from_min;
        // clang-format off
        lua_pushnumber(state, span == 0.0 ? to_min : to_min + (value -
                from_min) / span * (to_max - to_min));
        // clang-format on
        return 1;
    }
    if (name.ends_with(".clamp") && lua_gettop(state) >= 4) {
        const auto value = lua_tonumber(state, 2);
        const auto minimum = lua_tonumber(state, 3);
        const auto maximum = lua_tonumber(state, 4);
        lua_pushnumber(state, std::clamp(value, minimum, maximum));
        return 1;
    }
    lua_settop(state, 1);
    return 1;
}

int lua_proxy_index(lua_State *state) {
    lua_getfield(state, 1, "__nui_name");
    const char *parent = lua_tostring(state, -1);
    const char *field = lua_tostring(state, 2);
    std::string name = parent ? parent : "external";
    if (field && *field) {
        name.push_back('.');
        name.append(field);
    }
    lua_pop(state, 1);
    push_lua_proxy(state, name);
    return 1;
}

int lua_proxy_length(lua_State *state) {
    lua_pushinteger(state, 0);
    return 1;
}

int lua_proxy_compare(lua_State *state) {
    lua_pushboolean(state, false);
    return 1;
}

void push_lua_proxy(lua_State *state, const std::string_view name) {
    lua_newtable(state);
    lua_pushlstring(state, name.data(), name.size());
    lua_setfield(state, -2, "__nui_name");
    lua_newtable(state);
    lua_pushcfunction(state, lua_proxy_index);
    lua_setfield(state, -2, "__index");
    for (const char *method :
            { "__call", "__add", "__sub", "__mul", "__div", "__concat" }) {
        lua_pushcfunction(state, lua_proxy_identity);
        lua_setfield(state, -2, method);
    }
    lua_pushcfunction(state, lua_proxy_length);
    lua_setfield(state, -2, "__len");
    for (const char *name : { "__eq", "__lt", "__le" }) {
        lua_pushcfunction(state, lua_proxy_compare);
        lua_setfield(state, -2, name);
    }
    lua_setmetatable(state, -2);
}

int lua_proxy_loader(lua_State *state) {
    const char *module = lua_tostring(state, 1);
    push_lua_proxy(state, module ? module : "external");
    return 1;
}

int lua_partial_invoke(lua_State *state) {
    const auto forwarded = lua_gettop(state);
    lua_pushvalue(state, lua_upvalueindex(1));
    const auto bound = lua_rawlen(state, lua_upvalueindex(2));
    for (std::size_t index = 1; index <= bound; ++index)
        lua_rawgeti(
                state, lua_upvalueindex(2), static_cast<lua_Integer>(index));
    for (int index = 1; index <= forwarded; ++index)
        lua_pushvalue(state, index);
    lua_call(state, static_cast<int>(bound) + forwarded, LUA_MULTRET);
    return lua_gettop(state) - forwarded;
}

int lua_native_ui_partial(lua_State *state) {
    luaL_argcheck(state, !lua_isnoneornil(state, 1), 1,
            "partial requires a callback");
    lua_pushvalue(state, 1);
    lua_newtable(state);
    for (int index = 2; index <= lua_gettop(state) - 2; ++index) {
        lua_pushvalue(state, index);
        lua_rawseti(state, -2, index - 1);
    }
    lua_pushcclosure(state, lua_partial_invoke, 2);
    return 1;
}

int lua_native_ui_util_loader(lua_State *state) {
    push_lua_proxy(state, "native_ui_util");
    lua_pushcfunction(state, lua_native_ui_partial);
    lua_setfield(state, -2, "partial");
    return 1;
}

int lua_state_read(lua_State *state) {
    lua_getfield(state, lua_upvalueindex(1), "value");
    if (std::getenv("EKPHORE_PROFILE_CIRCULAR_DRAG") &&
            lua_isnumber(state, -1) &&
            (lua_tonumber(state, -1) == 216.0 ||
                    lua_tonumber(state, -1) == 46.0))
        std::fprintf(stderr, "native-state-read %p=%g\n",
                lua_topointer(state, lua_upvalueindex(1)),
                lua_tonumber(state, -1));
    return 1;
}

std::string lua_call_site(lua_State *state);

int lua_state_write(lua_State *state) {
    lua_getfield(state, LUA_REGISTRYINDEX, "ekphore.native_ui.runtime");
    auto *runtime = static_cast<NativeUiRuntime *>(lua_touserdata(state, -1));
    lua_pop(state, 1);
    // constructors run again on expansion, an unchanged init must NOT wipe
    // the user's edits
    if (runtime && runtime->materializing && !lua_istable(state, 1) &&
            !lua_isfunction(state, 1) && !lua_isuserdata(state, 1)) {
        const auto site = lua_call_site(state);
        lua_getfield(state, lua_upvalueindex(1), "construction_writes");
        if (!lua_istable(state, -1)) {
            lua_pop(state, 1);
            lua_newtable(state);
            lua_pushvalue(state, -1);
            lua_setfield(state, lua_upvalueindex(1), "construction_writes");
        }
        lua_getfield(state, -1, site.c_str());
        const bool seen = lua_istable(state, -1);
        if (seen) {
            lua_rawgeti(state, -1, 1);
            const bool same = lua_rawequal(state, 1, -1);
            lua_pop(state, 1);
            if (same) {
                lua_pop(state, 2);
                return 0;
            }
        }
        lua_pop(state, 1);
        lua_newtable(state);
        lua_pushvalue(state, 1);
        lua_rawseti(state, -2, 1);
        lua_setfield(state, -2, site.c_str());
        lua_pop(state, 1);
    }
    if (std::getenv("EKPHORE_PROFILE_CIRCULAR_DRAG"))
        std::fprintf(stderr, "native-state-write %p=%s\n",
                lua_topointer(state, lua_upvalueindex(1)),
                luaL_tolstring(state, 1, nullptr));
    lua_getfield(state, lua_upvalueindex(1), "value");
    const bool changed = !lua_rawequal(state, 1, -1);
    lua_pop(state, 1);
    if (changed) {
        if (runtime)
            ++runtime->computed_epoch;
    }
    lua_pushvalue(state, 1);
    lua_setfield(state, lua_upvalueindex(1), "value");
    return 0;
}

// factories run on every snapshot, so key state by owner and call site, the
// hook count gets mixed up between components
std::string lua_call_site(lua_State *state) {
    std::string result;
    lua_Debug info{};
    for (int level = 1; lua_getstack(state, level, &info); ++level) {
        lua_getinfo(state, "Sl", &info);
        if (info.what && std::string_view(info.what) == "C")
            break;
        result += "/";
        result += info.source ? info.source : "?";
        result += ":" + std::to_string(info.currentline);
    }
    return result;
}

int lua_create_state(lua_State *state) {
    lua_getfield(state, LUA_REGISTRYINDEX, "ekphore.native_ui.runtime");
    auto *runtime = static_cast<NativeUiRuntime *>(lua_touserdata(state, -1));
    lua_pop(state, 1);
    std::string key;
    if (runtime) {
        key = runtime->state_owner + "/state" + lua_call_site(state);
        key += "#" + std::to_string(runtime->instance_occurrences[key]++);
    }
    const auto existing = runtime ? runtime->state_references.find(key)
                                  : std::map<std::string, int>::iterator{};
    if (runtime && existing != runtime->state_references.end()) {
        lua_rawgeti(state, LUA_REGISTRYINDEX, existing->second);
    } else {
        lua_newtable(state);
        lua_pushvalue(state, 1);
        lua_setfield(state, -2, "value");
        if (runtime) {
            lua_pushvalue(state, -1);
            runtime->state_references.emplace(
                    key, luaL_ref(state, LUA_REGISTRYINDEX));
        }
    }
    lua_pushvalue(state, -1);
    lua_pushcclosure(state, lua_state_read, 1);
    lua_pushvalue(state, -2);
    lua_pushcclosure(state, lua_state_write, 1);
    return 2;
}

int lua_create_computed(lua_State *state);

int lua_create_context(lua_State *state) {
    const auto has_default = lua_gettop(state) > 0;
    lua_newtable(state);
    if (has_default) {
        lua_pushvalue(state, 1);
        lua_setfield(state, -2, "value");
    }
    lua_getfield(state, LUA_REGISTRYINDEX, "ekphore.native_ui.runtime");
    auto *runtime = static_cast<NativeUiRuntime *>(lua_touserdata(state, -1));
    lua_pop(state, 1);
    if (runtime) {
        lua_pushvalue(state, -1);
        runtime->context_keys.push_back(luaL_ref(state, LUA_REGISTRYINDEX));
    }
    return 1;
}

int lua_context_read(lua_State *state) {
    lua_getfield(state, lua_upvalueindex(1), "value");
    if (lua_isnil(state, -1)) {
        lua_pop(state, 1);
        lua_pushinteger(state, 0);
    }
    return 1;
}

int lua_use_context(lua_State *state) {
    // the provider we see NOW, a later sibling overrides the shared one
    lua_newtable(state);
    lua_getfield(state, 1, "value");
    lua_setfield(state, -2, "value");
    lua_pushcclosure(state, lua_context_read, 1);
    return 1;
}

int lua_table_shallow_copy(lua_State *state) {
    luaL_checktype(state, 1, LUA_TTABLE);
    lua_newtable(state);
    lua_pushnil(state);
    while (lua_next(state, 1) != 0) {
        lua_pushvalue(state, -2);
        lua_pushvalue(state, -2);
        lua_settable(state, -5);
        lua_pop(state, 1);
    }
    return 1;
}

struct LuaParameterContext {
    NativeUiRuntime *runtime{};
};

void push_parameter_value(lua_State *state, const NativeUiParameterValue &value,
        const std::optional<std::size_t> index = {}) {
    std::visit(
            [&](const auto &item) {
                using Value = std::decay_t<decltype(item)>;
                if constexpr (std::is_same_v<Value, bool>)
                    lua_pushboolean(state, item);
                else if constexpr (std::is_same_v<Value, double>)
                    lua_pushnumber(state, item);
                else if constexpr (std::is_same_v<Value, std::int64_t>)
                    lua_pushinteger(state, item);
                else if constexpr (std::is_same_v<Value, std::string>)
                    lua_pushlstring(state, item.data(), item.size());
                else {
                    if (index && *index < item.size()) {
                        if constexpr (std::is_same_v<Value,
                                              std::vector<double>>)
                            lua_pushnumber(state, item[*index]);
                        else
                            lua_pushlstring(state, item[*index].data(),
                                    item[*index].size());
                    } else if (index) {
                        lua_pushinteger(state, 0);
                    } else {
                        lua_newtable(state);
                        for (std::size_t i = 0; i < item.size(); ++i) {
                            if constexpr (std::is_same_v<Value,
                                                  std::vector<double>>)
                                lua_pushnumber(state, item[i]);
                            else
                                lua_pushlstring(
                                        state, item[i].data(), item[i].size());
                            lua_rawseti(
                                    state, -2, static_cast<lua_Integer>(i + 1));
                        }
                    }
                }
            },
            value);
}

int lua_parameter_value(lua_State *state) {
    const auto *context = static_cast<const LuaParameterContext *>(
            lua_touserdata(state, lua_upvalueindex(1)));
    const char *name = lua_tostring(state, lua_upvalueindex(2));
    if (!context || !context->runtime || !context->runtime->source || !name) {
        lua_pushinteger(state, 0);
        return 1;
    }
    const auto value = context->runtime->pending_source &&
                                       !context->runtime->writes.empty()
                               ? context->runtime->pending_source(
                                         name, context->runtime->writes)
                               : context->runtime->source(name);
    if (!value) {
        lua_pushinteger(state, 0);
        return 1;
    }
    if (const auto *filter = std::getenv("EKPHORE_UI_TRACE_PARAMETER");
            filter &&
            std::string_view(name).find(filter) != std::string_view::npos) {
        std::visit(
                [&](const auto &v) {
                    if constexpr (std::is_arithmetic_v<
                                          std::decay_t<decltype(v)>>)
                        detail::gesture_trace(
                                "lua-read parameter=%s value=%.17g pending-writes=%zu",
                                name, static_cast<double>(v),
                                context->runtime->writes.size());
                },
                *value);
    }
    std::optional<std::size_t> index;
    // numbers, even 0.0 when it's used as an array index
    if (lua_gettop(state) >= 2 && lua_isnumber(state, 2))
        index = static_cast<std::size_t>(
                std::max<lua_Integer>(lua_tointeger(state, 2), 0));
    push_parameter_value(state, *value, index);
    return 1;
}

std::optional<NativeUiParameterValue> lua_parameter_argument(
        lua_State *state, const int index) {
    switch (lua_type(state, index)) {
        case LUA_TBOOLEAN:
            return NativeUiParameterValue{ lua_toboolean(state, index) != 0 };
        case LUA_TNUMBER:
            return NativeUiParameterValue{ static_cast<double>(
                    lua_tonumber(state, index)) };
        case LUA_TSTRING: {
            std::size_t size{};
            const char *text = lua_tolstring(state, index, &size);
            return NativeUiParameterValue{ std::string(text, size) };
        }
        default:
            return std::nullopt;
    }
}

int lua_parameter_set_value(lua_State *state) {
    auto *context = static_cast<LuaParameterContext *>(
            lua_touserdata(state, lua_upvalueindex(1)));
    const char *name = lua_tostring(state, lua_upvalueindex(2));
    if (context && context->runtime && name)
        if (auto value = lua_parameter_argument(
                    state, lua_gettop(state) >= 2 ? 2 : 1)) {
            std::visit(
                    [&](const auto &v) {
                        if constexpr (std::is_arithmetic_v<
                                              std::decay_t<decltype(v)>>)
                            detail::gesture_trace(
                                    "lua-write parameter=%s value=%.17g", name,
                                    static_cast<double>(v));
                    },
                    *value);
            const bool indexed = lua_gettop(state) >= 3 &&
                                 lua_isinteger(state, 3) &&
                                 lua_tointeger(state, 3) >= 0;
            const auto index =
                    indexed ? std::optional<std::size_t>{ static_cast<
                                      std::size_t>(lua_tointeger(state, 3)) }
                            : std::nullopt;
            context->runtime->writes.push_back(
                    { name, std::move(*value), indexed, index });
            ++context->runtime->computed_epoch;
        }
    return 0;
}

int lua_parameter_touch(lua_State *) {
    // only set_value actually changes it, these are just gesture notifications
    return 0;
}

int lua_parameter_ksp_control_property(lua_State *state) {
    const auto *context = static_cast<const LuaParameterContext *>(
            lua_touserdata(state, lua_upvalueindex(1)));
    const char *name = lua_tostring(state, lua_upvalueindex(2));
    const auto property = luaL_optinteger(state, 2, -1);
    const auto index = luaL_optinteger(state, 3, -1);
    if (context && context->runtime && name &&
            context->runtime->control_properties) {
        if (const auto value = context->runtime->control_properties(name,
                    static_cast<std::int32_t>(property),
                    static_cast<std::int32_t>(index))) {
            push_parameter_value(state, *value);
            return 1;
        }
    }
    // empty count for this one, nil for anything else we don't support
    if (property == 17)
        lua_pushinteger(state, 0);
    else
        lua_pushnil(state);
    return 1;
}

int lua_connection_parameter(lua_State *state) {
    const char *name = luaL_checkstring(state, 1);
    lua_newtable(state);
    lua_pushvalue(state, lua_upvalueindex(1));
    lua_pushstring(state, name);
    lua_pushcclosure(state, lua_parameter_value, 2);
    lua_setfield(state, -2, "value");
    lua_pushvalue(state, lua_upvalueindex(1));
    lua_pushstring(state, name);
    lua_pushcclosure(state, lua_parameter_set_value, 2);
    lua_setfield(state, -2, "set_value");
    lua_pushcfunction(state, lua_parameter_touch);
    lua_setfield(state, -2, "update_touch");
    lua_pushcfunction(state, lua_parameter_touch);
    lua_setfield(state, -2, "end_touch");
    lua_pushvalue(state, lua_upvalueindex(1));
    lua_pushstring(state, name);
    lua_pushcclosure(state, lua_parameter_ksp_control_property, 2);
    lua_setfield(state, -2, "ksp_control_property");
    return 1;
}

int lua_level_meter_value(lua_State *state) {
    const auto *context = static_cast<const LuaParameterContext *>(
            lua_touserdata(state, lua_upvalueindex(1)));
    const char *name = lua_tostring(state, lua_upvalueindex(2));
    if (context && context->runtime && context->runtime->source && name) {
        if (const auto value = context->runtime->source(name)) {
            if (const auto *scalar = std::get_if<double>(&*value)) {
                lua_pushnumber(state, *scalar);
                return 1;
            }
        }
    }
    lua_pushnumber(state, 0.0);
    return 1;
}

int lua_connection_level_meter(lua_State *state) {
    const char *name = luaL_checkstring(state, 1);
    lua_newtable(state);
    lua_pushvalue(state, lua_upvalueindex(1));
    lua_pushstring(state, name);
    lua_pushcclosure(state, lua_level_meter_value, 2);
    lua_setfield(state, -2, "level_value");
    return 1;
}

int lua_connection_loader(lua_State *state) {
    lua_newtable(state);
    lua_pushvalue(state, lua_upvalueindex(1));
    lua_pushcclosure(state, lua_connection_parameter, 1);
    lua_setfield(state, -2, "parameter");
    lua_pushvalue(state, lua_upvalueindex(1));
    lua_pushcclosure(state, lua_connection_parameter, 1);
    lua_setfield(state, -2, "connect_parameter");
    lua_pushvalue(state, lua_upvalueindex(1));
    lua_pushcclosure(state, lua_connection_level_meter, 1);
    lua_setfield(state, -2, "connect_level_meter");
    return 1;
}

void push_native_color(lua_State *state, const std::uint32_t argb);

int lua_native_color_opacity(lua_State *state) {
    lua_getfield(state, 1, "__nui_color");
    const auto color = static_cast<std::uint32_t>(lua_tointeger(state, -1));
    lua_pop(state, 1);
    const auto opacity = std::clamp(luaL_optnumber(state, 2, 1.0), 0.0, 1.0);
    const auto alpha = static_cast<std::uint32_t>(
            std::lround(static_cast<double>((color >> 24U) & 0xffU) * opacity));
    push_native_color(state, (color & 0x00ffffffU) | (alpha << 24U));
    return 1;
}

int lua_native_color_value_index(lua_State *state) {
    const char *field = lua_tostring(state, 2);
    if (field && std::string_view(field) == "opacity") {
        lua_pushcfunction(state, lua_native_color_opacity);
        return 1;
    }
    lua_pushnil(state);
    return 1;
}

void push_native_color(lua_State *state, const std::uint32_t argb) {
    lua_newtable(state);
    lua_pushinteger(state, static_cast<lua_Integer>(argb));
    lua_setfield(state, -2, "__nui_color");
    lua_newtable(state);
    lua_pushcfunction(state, lua_native_color_value_index);
    lua_setfield(state, -2, "__index");
    lua_setmetatable(state, -2);
}

int lua_native_color_call(lua_State *state) {
    const auto count = lua_gettop(state) - 1;
    if (count == 3 || count == 4) {
        const auto channel = [state](const int index) {
            return static_cast<std::uint32_t>(std::clamp<lua_Integer>(
                    luaL_checkinteger(state, index), 0, 255));
        };
        const auto red = channel(2);
        const auto green = channel(3);
        const auto blue = channel(4);
        const auto alpha = count == 4 ? channel(5) : 255U;
        push_native_color(
                state, (alpha << 24U) | (red << 16U) | (green << 8U) | blue);
    } else {
        push_native_color(
                state, static_cast<std::uint32_t>(luaL_checkinteger(state, 2)));
    }
    return 1;
}

int lua_native_color_namespace_index(lua_State *state) {
    const char *field = lua_tostring(state, 2);
    const std::string_view name = field ? field : "";
    if (name == "none")
        push_native_color(state, 0x00000000U);
    else if (name == "black")
        push_native_color(state, 0xff000000U);
    else if (name == "white")
        push_native_color(state, 0xffffffffU);
    else if (name == "red")
        push_native_color(state, 0xffff0000U);
    else if (name == "green")
        push_native_color(state, 0xff00ff00U);
    else if (name == "blue")
        push_native_color(state, 0xff0000ffU);
    // both spellings work
    else if (name == "gray" || name == "grey")
        push_native_color(state, 0xff808080U);
    else
        lua_pushnil(state);
    return 1;
}

void push_native_color_namespace(lua_State *state) {
    lua_newtable(state);
    lua_newtable(state);
    lua_pushcfunction(state, lua_native_color_call);
    lua_setfield(state, -2, "__call");
    lua_pushcfunction(state, lua_native_color_namespace_index);
    lua_setfield(state, -2, "__index");
    lua_setmetatable(state, -2);
}

int lua_native_ui_loader(lua_State *state) {
    push_lua_proxy(state, "native_ui");
    push_native_color_namespace(state);
    lua_setfield(state, -2, "Color");
    // don't round these, ImageButton checks release against them
    lua_pushcfunction(state, ([](lua_State *callback) {
        const double values[]{ luaL_checknumber(callback, 1),
            luaL_checknumber(callback, 2), luaL_checknumber(callback, 3),
            luaL_checknumber(callback, 4) };
        lua_createtable(callback, 0, 4);
        constexpr const char *fields[]{ "x", "y", "width", "height" };
        for (std::size_t index = 0; index < 4; ++index) {
            lua_pushnumber(callback, values[index]);
            lua_setfield(callback, -2, fields[index]);
        }
        return 1;
    }));
    lua_setfield(state, -2, "Rect");
    for (const auto &[name, function] :
            std::initializer_list<std::pair<const char *, lua_CFunction>>{
                    { "create_state", lua_create_state },
                    { "create_computed", lua_create_computed },
                    { "create_context", lua_create_context },
                    { "use_context", lua_use_context } }) {
        lua_pushcfunction(state, function);
        lua_setfield(state, -2, name);
    }
    lua_pushcfunction(state, [](lua_State *callback) {
        lua_settop(callback, 1);
        return 1;
    });
    lua_setfield(state, -2, "load_font");
    return 1;
}

void compact_positional_values(lua_State *state, const int source) {
    const auto profile_start = std::chrono::steady_clock::now();
    ++profile_compact_count;
    const int table = lua_absindex(state, source);
    std::vector<std::pair<lua_Integer, int>> values;
    lua_pushnil(state);
    while (lua_next(state, table) != 0) {
        if (lua_isinteger(state, -2)) {
            const auto key = lua_tointeger(state, -2);
            if (key > 0 && !lua_isnil(state, -1)) {
                lua_pushvalue(state, -1);
                values.emplace_back(key, luaL_ref(state, LUA_REGISTRYINDEX));
            }
        }
        lua_pop(state, 1);
    }
    std::ranges::sort(values, {}, &decltype(values)::value_type::first);
    for (const auto &[key, reference] : values) {
        (void)reference;
        lua_pushnil(state);
        lua_rawseti(state, table, key);
    }
    lua_Integer destination{};
    for (const auto &[key, reference] : values) {
        (void)key;
        lua_rawgeti(state, LUA_REGISTRYINDEX, reference);
        bool collection = lua_istable(state, -1);
        if (collection) {
            lua_getfield(state, -1, "__nui_kind");
            collection = lua_isnil(state, -1);
            lua_pop(state, 1);
        }
        if (collection) {
            lua_getfield(state, -1, "__nui_factory");
            collection = lua_isnil(state, -1);
            lua_pop(state, 1);
        }
        if (collection) {
            const int children = lua_absindex(state, -1);
            for (std::size_t child = 1; child <= lua_rawlen(state, children);
                    ++child) {
                lua_rawgeti(state, children, static_cast<lua_Integer>(child));
                lua_rawseti(state, table, ++destination);
            }
            lua_pop(state, 1);
        } else {
            lua_rawseti(state, table, ++destination);
        }
        luaL_unref(state, LUA_REGISTRYINDEX, reference);
    }
    profile_compact_us += std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - profile_start)
                                  .count();
}

int lua_native_component(lua_State *state) {
    if (lua_isfunction(state, 1)) {
        if (!lua_istable(state, 2))
            lua_newtable(state);
        const int props = lua_absindex(state, 2);
        compact_positional_values(state, props);
        lua_getfield(state, props, "children");
        const bool has_children = !lua_isnil(state, -1);
        lua_pop(state, 1);
        if (!has_children) {
            lua_newtable(state);
            const int children = lua_absindex(state, -1);
            for (std::size_t i = 1; i <= lua_rawlen(state, props); ++i) {
                lua_rawgeti(state, props, static_cast<lua_Integer>(i));
                lua_rawseti(state, children, static_cast<lua_Integer>(i));
            }
            lua_setfield(state, props, "children");
        }
        lua_newtable(state);
        lua_pushvalue(state, 1);
        lua_setfield(state, -2, "__nui_factory");
        lua_pushvalue(state, props);
        lua_setfield(state, -2, "__nui_props");
        lua_getfield(state, LUA_REGISTRYINDEX, "ekphore.native_ui.runtime");
        auto *runtime =
                static_cast<NativeUiRuntime *>(lua_touserdata(state, -1));
        lua_pop(state, 1);
        if (runtime) {
            lua_pushvalue(state, 1);
            lua_Debug info{};
            lua_getinfo(state, ">S", &info);
            auto key = runtime->state_owner + "/component" +
                       lua_call_site(state) + "/" +
                       (info.source ? info.source : "?") + ":" +
                       std::to_string(info.linedefined);
            key += "#" + std::to_string(runtime->instance_occurrences[key]++);
            lua_pushlstring(state, key.data(), key.size());
            lua_setfield(state, -2, "__nui_owner");
        }
        return 1;
    }
    if (!lua_istable(state, 2))
        lua_newtable(state);
    compact_positional_values(state, 2);
    lua_getfield(state, 1, "__nui_name");
    if (lua_isstring(state, -1))
        lua_setfield(state, 2, "__nui_kind");
    else
        lua_pop(state, 1);
    lua_pushvalue(state, 2);
    return 1;
}

struct ScopedFactoryContext {
    lua_State *state;
    std::vector<std::pair<int, int>> saved;
    ~ScopedFactoryContext() {
        for (auto it = saved.rbegin(); it != saved.rend(); ++it) {
            lua_rawgeti(state, LUA_REGISTRYINDEX, it->first);
            if (it->second == LUA_REFNIL)
                lua_pushnil(state);
            else
                lua_rawgeti(state, LUA_REGISTRYINDEX, it->second);
            lua_setfield(state, -2, "value");
            lua_pop(state, 1);
            luaL_unref(state, LUA_REGISTRYINDEX, it->first);
            luaL_unref(state, LUA_REGISTRYINDEX, it->second);
        }
    }
};

int lua_computed_read(lua_State *state) {
    const int arguments = lua_gettop(state);
    lua_getfield(state, LUA_REGISTRYINDEX, "ekphore.native_ui.runtime");
    auto *runtime = static_cast<NativeUiRuntime *>(lua_touserdata(state, -1));
    lua_pop(state, 1);
    if (runtime && arguments == 0) {
        lua_getfield(state, lua_upvalueindex(3), "epoch");
        const bool cached = lua_isinteger(state, -1) &&
                            static_cast<std::uint64_t>(lua_tointeger(
                                    state, -1)) == runtime->computed_epoch;
        lua_pop(state, 1);
        if (cached) {
            lua_getfield(state, lua_upvalueindex(3), "count");
            const int count = static_cast<int>(lua_tointeger(state, -1));
            lua_pop(state, 1);
            for (int i = 1; i <= count; ++i)
                lua_rawgeti(state, lua_upvalueindex(3), i);
            return count;
        }
    }
    int status{};
    {
        ScopedFactoryContext scope{ state, {} };
        // put back what we had when this was created, nil included, NOT
        // whatever a sibling left
        for (std::size_t i = 1; i <= lua_rawlen(state, lua_upvalueindex(2));
                ++i) {
            lua_rawgeti(
                    state, lua_upvalueindex(2), static_cast<lua_Integer>(i));
            lua_rawgeti(state, -1, 1);
            lua_pushvalue(state, -1);
            const int key = luaL_ref(state, LUA_REGISTRYINDEX);
            lua_getfield(state, -1, "value");
            const int previous = luaL_ref(state, LUA_REGISTRYINDEX);
            scope.saved.emplace_back(key, previous);
            lua_rawgeti(state, -2, 2);
            lua_setfield(state, -2, "value");
            lua_pop(state, 2);
        }
        lua_pushvalue(state, lua_upvalueindex(1));
        lua_insert(state, 1);
        status = lua_pcall(state, arguments, LUA_MULTRET, 0);
    } // restore even if the getter errors, never longjmp over C++ cleanup
    if (status != LUA_OK)
        return lua_error(state);
    if (runtime && arguments == 0) {
        const int count = lua_gettop(state);
        for (int i = 1; i <= count; ++i) {
            lua_pushvalue(state, i);
            lua_rawseti(state, lua_upvalueindex(3), i);
        }
        lua_pushinteger(state, count);
        lua_setfield(state, lua_upvalueindex(3), "count");
        lua_pushinteger(
                state, static_cast<lua_Integer>(runtime->computed_epoch));
        lua_setfield(state, lua_upvalueindex(3), "epoch");
    }
    return lua_gettop(state);
}

int lua_create_computed(lua_State *state) {
    lua_settop(state, 1);
    lua_getfield(state, LUA_REGISTRYINDEX, "ekphore.native_ui.runtime");
    const auto *runtime =
            static_cast<NativeUiRuntime *>(lua_touserdata(state, -1));
    lua_pop(state, 1);
    lua_newtable(state);
    if (runtime) {
        std::size_t index{};
        for (const int reference : runtime->context_keys) {
            lua_newtable(state);
            lua_rawgeti(state, LUA_REGISTRYINDEX, reference);
            lua_getfield(state, -1, "value");
            lua_rawseti(state, -3, 2);
            lua_rawseti(state, -2, 1);
            lua_rawseti(state, -2, static_cast<lua_Integer>(++index));
        }
    }
    lua_newtable(state);
    lua_pushcclosure(state, lua_computed_read, 3);
    return 1;
}

void apply_factory_context(
        lua_State *state, const int factory_node, ScopedFactoryContext &scope) {
    const int node = lua_absindex(state, factory_node);
    lua_getfield(state, node, "__nui_modifiers");
    if (!lua_istable(state, -1)) {
        lua_pop(state, 1);
        return;
    }
    const int modifiers = lua_absindex(state, -1);
    for (std::size_t i = 1; i <= lua_rawlen(state, modifiers); ++i) {
        lua_rawgeti(state, modifiers, static_cast<lua_Integer>(i));
        lua_getfield(state, -1, "name");
        const char *name = lua_tostring(state, -1);
        lua_pop(state, 1);
        if (name && std::string_view(name) == "context") {
            lua_getfield(state, -1, "arguments");
            lua_getfield(state, -1, "key");
            lua_getfield(state, -2, "value");
            if (lua_istable(state, -2)) {
                lua_pushvalue(state, -2);
                const auto key = luaL_ref(state, LUA_REGISTRYINDEX);
                lua_getfield(state, -2, "value");
                const auto previous = luaL_ref(state, LUA_REGISTRYINDEX);
                scope.saved.emplace_back(key, previous);
                lua_pushvalue(state, -1);
                lua_setfield(state, -3, "value");
            }
            lua_pop(state, 3);
        }
        lua_pop(state, 1);
    }
    lua_pop(state, 1);
}

bool factory_is_hidden(lua_State *state, const int factory_node) {
    const int node = lua_absindex(state, factory_node);
    lua_getfield(state, node, "__nui_modifiers");
    if (!lua_istable(state, -1)) {
        lua_pop(state, 1);
        return false;
    }
    const int modifiers = lua_absindex(state, -1);
    bool hidden{};
    for (std::size_t i = 1; i <= lua_rawlen(state, modifiers); ++i) {
        lua_rawgeti(state, modifiers, static_cast<lua_Integer>(i));
        lua_getfield(state, -1, "name");
        const char *name = lua_tostring(state, -1);
        lua_pop(state, 1);
        if (name && std::string_view(name) == "hidden") {
            lua_getfield(state, -1, "arguments");
            lua_rawgeti(state, -1, 1);
            hidden = lua_toboolean(state, -1) != 0;
            lua_pop(state, 2);
        }
        lua_pop(state, 1);
        if (hidden)
            break;
    }
    lua_pop(state, 1);
    return hidden;
}

int lua_return_upvalue(lua_State *state) {
    lua_pushvalue(state, lua_upvalueindex(1));
    return 1;
}

void push_value_accessor(lua_State *state, const int value) {
    lua_pushvalue(state, value);
    lua_pushcclosure(state, lua_return_upvalue, 1);
}

void push_list_item_state(lua_State *state, const std::size_t index,
        const std::size_t selected_index) {
    lua_newtable(state);
    lua_pushinteger(state, static_cast<lua_Integer>(index));
    lua_pushcclosure(state, lua_return_upvalue, 1);
    lua_setfield(state, -2, "index");
    lua_pushboolean(state, index == selected_index);
    lua_pushcclosure(state, lua_return_upvalue, 1);
    lua_setfield(state, -2, "selected");
    lua_pushboolean(state, false);
    lua_pushcclosure(state, lua_return_upvalue, 1);
    lua_setfield(state, -2, "hovered");
}

bool push_materialized(lua_State *state, const int source,
        std::string &diagnostic, const unsigned depth = 0) {
    if (depth > 256) {
        diagnostic = "native UI component nesting exceeds 256";
        return false;
    }
    const int absolute = lua_absindex(state, source);
    if (!lua_istable(state, absolute)) {
        lua_pushvalue(state, absolute);
        return true;
    }
    // BEFORE expanding children, factories in native containers included
    ScopedFactoryContext context_scope{ state, {} };
    apply_factory_context(state, absolute, context_scope);
    lua_getfield(state, absolute, "__nui_factory");
    const bool factory = lua_isfunction(state, -1);
    lua_pop(state, 1);
    if (factory) {
        // factories are lazy, a hidden branch might need stuff that isn't
        // there so don't evaluate it
        if (factory_is_hidden(state, absolute)) {
            lua_pushvalue(state, absolute);
            return true;
        }
        lua_getfield(state, LUA_REGISTRYINDEX, "ekphore.native_ui.runtime");
        auto *runtime =
                static_cast<NativeUiRuntime *>(lua_touserdata(state, -1));
        lua_pop(state, 1);
        struct OwnerScope {
            NativeUiRuntime *runtime;
            std::string previous;
            ~OwnerScope() {
                if (runtime)
                    runtime->state_owner = std::move(previous);
            }
        } owner_scope{ runtime,
            runtime ? runtime->state_owner : std::string{} };
        lua_getfield(state, absolute, "__nui_owner");
        if (runtime && lua_isstring(state, -1))
            runtime->state_owner = lua_tostring(state, -1);
        lua_pop(state, 1);
        lua_getfield(state, absolute, "__nui_factory");
        lua_getfield(state, absolute, "__nui_props");
        lua_pushcfunction(state, [](lua_State *error_state) {
            const char *message = lua_tostring(error_state, 1);
            luaL_traceback(error_state, error_state,
                    message ? message : "factory error", 1);
            return 1;
        });
        lua_insert(state, -3);
        const auto profile_factory_start = std::chrono::steady_clock::now();
        ++profile_factory_count;
        const auto factory_result =
                lua_pcall(state, 1, 1, lua_absindex(state, -3));
        lua_remove(state, -2);
        profile_factory_us +=
                std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() -
                        profile_factory_start)
                        .count();
        if (factory_result != LUA_OK) {
            lua_Debug factory_info{};
            lua_getfield(state, absolute, "__nui_factory");
            const bool have_factory_info =
                    lua_getinfo(state, ">S", &factory_info) != 0;
            const char *message = lua_tostring(state, -1);
            luaL_traceback(state, state,
                    message ? message : "unknown factory error", 1);
            const char *traceback = lua_tostring(state, -1);
            // clang-format off
            diagnostic = have_factory_info ?
                    std::string(factory_info.short_src) + ":" +
                    std::to_string(factory_info.linedefined) + ": " +
                    (traceback ? traceback : "unknown factory error") :
                    (traceback ? traceback : "unknown factory error");
            // clang-format on
            lua_pop(state, 1);
            lua_pop(state, 1);
            return false;
        }
        const int produced = lua_absindex(state, -1);
        if (lua_istable(state, produced)) {
            // call site modifiers wrap the returned view, merging them in
            // breaks offset/alignment
            lua_getfield(state, absolute, "__nui_modifiers");
            if (lua_istable(state, -1) && lua_rawlen(state, -1) != 0) {
                const int outer_modifiers = lua_absindex(state, -1);
                bool layout_wrapper = false;
                for (std::size_t i = 1; i <= lua_rawlen(state, outer_modifiers);
                        ++i) {
                    lua_rawgeti(state, outer_modifiers,
                            static_cast<lua_Integer>(i));
                    lua_getfield(state, -1, "name");
                    const char *name = lua_tostring(state, -1);
                    layout_wrapper = name && std::string_view(name) == "offset";
                    lua_pop(state, 2);
                    if (layout_wrapper)
                        break;
                }
                if (layout_wrapper) {
                    lua_newtable(state);
                    const int wrapper = lua_absindex(state, -1);
                    lua_pushstring(state, "__ekphore.ModifierWrapper");
                    lua_setfield(state, wrapper, "__nui_kind");
                    lua_pushvalue(state, outer_modifiers);
                    lua_setfield(state, wrapper, "__nui_modifiers");
                    lua_pushvalue(state, produced);
                    lua_rawseti(state, wrapper, 1);
                    if (!push_materialized(
                                state, wrapper, diagnostic, depth + 1)) {
                        lua_pop(state, 1);
                        return false;
                    }
                    lua_remove(state, wrapper);
                    lua_remove(state, outer_modifiers);
                    lua_remove(state, produced);
                    return true;
                }
                lua_getfield(state, produced, "__nui_modifiers");
                if (!lua_istable(state, -1)) {
                    lua_pop(state, 1);
                    lua_newtable(state);
                }
                const int combined_modifiers = lua_absindex(state, -1);
                auto destination = lua_rawlen(state, combined_modifiers);
                for (std::size_t i = 1; i <= lua_rawlen(state, outer_modifiers);
                        ++i) {
                    lua_rawgeti(state, outer_modifiers,
                            static_cast<lua_Integer>(i));
                    lua_rawseti(state, combined_modifiers,
                            static_cast<lua_Integer>(++destination));
                }
                lua_setfield(state, produced, "__nui_modifiers");
                lua_pop(state, 1);
            } else {
                lua_pop(state, 1);
            }
        }
        if (!push_materialized(state, produced, diagnostic, depth + 1)) {
            lua_pop(state, 1);
            return false;
        }
        lua_remove(state, produced);
        return true;
    }

    lua_getfield(state, absolute, "__nui_kind");
    const char *raw_kind = lua_tostring(state, -1);
    const auto kind =
            raw_kind ? std::string_view(raw_kind) : std::string_view{};
    const bool for_each = kind == "native_ui.ForEach";
    const bool list = kind.ends_with(".List") || kind == "List";
    lua_pop(state, 1);
    if (for_each && lua_rawlen(state, absolute) == 0) {
        lua_getfield(state, absolute, "data");
        lua_getfield(state, absolute, "content");
        const int data = lua_absindex(state, -2);
        const int content = lua_absindex(state, -1);
        if (lua_istable(state, data) && lua_isfunction(state, content)) {
            for (std::size_t i = 1; i <= lua_rawlen(state, data); ++i) {
                lua_pushvalue(state, content);
                lua_rawgeti(state, data, static_cast<lua_Integer>(i));
                lua_pushinteger(state, static_cast<lua_Integer>(i));
                lua_pushcclosure(
                        state,
                        [](lua_State *callback) {
                            lua_pushvalue(callback, lua_upvalueindex(1));
                            return 1;
                        },
                        1);
                const auto profile_foreach_start =
                        std::chrono::steady_clock::now();
                const auto foreach_result = lua_pcall(state, 2, 1, 0);
                profile_foreach_us +=
                        std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now() -
                                profile_foreach_start)
                                .count();
                if (foreach_result != LUA_OK) {
                    const char *message = lua_tostring(state, -1);
                    diagnostic = message ? message
                                         : "native UI ForEach content failed";
                    lua_pop(state, 3);
                    return false;
                }
                lua_rawseti(state, absolute, static_cast<lua_Integer>(i));
            }
        }
        lua_pop(state, 2);
    }

    if (list && lua_rawlen(state, absolute) == 0) {
        lua_getfield(state, absolute, "entries");
        lua_getfield(state, absolute, "delegate");
        const int entries = lua_absindex(state, -2);
        const int delegate = lua_absindex(state, -1);
        std::size_t selected_index{ 1 };
        lua_getfield(state, absolute, "current_index");
        if (lua_isnumber(state, -1))
            selected_index = static_cast<std::size_t>(
                    std::max<lua_Integer>(lua_tointeger(state, -1), 0));
        lua_pop(state, 1);
        if (lua_istable(state, entries) && lua_isfunction(state, delegate)) {
            for (std::size_t i = 1; i <= lua_rawlen(state, entries); ++i) {
                lua_pushvalue(state, delegate);
                lua_rawgeti(state, entries, static_cast<lua_Integer>(i));
                push_value_accessor(state, -1);
                lua_remove(state, -2);
                // one-based, Omnia relies on it for its footer sprites
                push_list_item_state(state, i, selected_index);
                if (lua_pcall(state, 2, 1, 0) != LUA_OK) {
                    const char *message = lua_tostring(state, -1);
                    diagnostic = message ? message
                                         : "native UI List delegate failed";
                    lua_pop(state, 3);
                    return false;
                }
                lua_rawseti(state, absolute, static_cast<lua_Integer>(i));
            }
        }
        lua_pop(state, 2);
    }

    // background/overlay can have factories too, expand them like children
    lua_getfield(state, absolute, "__nui_modifiers");
    if (lua_istable(state, -1)) {
        const int modifiers = lua_absindex(state, -1);
        for (std::size_t i = 1; i <= lua_rawlen(state, modifiers); ++i) {
            lua_rawgeti(state, modifiers, static_cast<lua_Integer>(i));
            lua_getfield(state, -1, "arguments");
            if (lua_istable(state, -1)) {
                const int arguments = lua_absindex(state, -1);
                // popover and friends pass it by name
                lua_getfield(state, arguments, "content");
                if (lua_istable(state, -1)) {
                    if (!push_materialized(state, -1, diagnostic, depth + 1)) {
                        lua_pop(state, 3);
                        return false;
                    }
                    lua_setfield(state, arguments, "content");
                }
                lua_pop(state, 1);
                for (std::size_t argument = 1;
                        argument <= lua_rawlen(state, arguments); ++argument) {
                    lua_rawgeti(state, arguments,
                            static_cast<lua_Integer>(argument));
                    if (lua_istable(state, -1)) {
                        if (!push_materialized(
                                    state, -1, diagnostic, depth + 1)) {
                            lua_pop(state, 3);
                            return false;
                        }
                        lua_rawseti(state, arguments,
                                static_cast<lua_Integer>(argument));
                    }
                    lua_pop(state, 1);
                }
            }
            lua_pop(state, 2);
        }
    }
    lua_pop(state, 1);

    const auto count = lua_rawlen(state, absolute);
    for (std::size_t i = 1; i <= count; ++i) {
        lua_rawgeti(state, absolute, static_cast<lua_Integer>(i));
        if (!push_materialized(state, -1, diagnostic, depth + 1)) {
            lua_pop(state, 1);
            return false;
        }
        lua_rawseti(state, absolute, static_cast<lua_Integer>(i));
        lua_pop(state, 1);
    }
    lua_pushvalue(state, absolute);
    return true;
}

int lua_native_modifier(lua_State *state) {
    if (!lua_istable(state, 1)) {
        lua_settop(state, 1);
        return 1;
    }
    lua_getfield(state, 1, "__nui_modifiers");
    if (!lua_istable(state, -1)) {
        lua_pop(state, 1);
        lua_newtable(state);
        lua_pushvalue(state, -1);
        lua_setfield(state, 1, "__nui_modifiers");
    }
    const auto next = lua_rawlen(state, -1) + 1;
    lua_newtable(state);
    lua_pushvalue(state, 2);
    lua_setfield(state, -2, "name");
    lua_pushvalue(state, 3);
    lua_setfield(state, -2, "arguments");
    lua_rawseti(state, -2, static_cast<lua_Integer>(next));
    lua_pop(state, 1);
    lua_pushvalue(state, 1);
    return 1;
}

void install_declarative_runtime(lua_State *state) {
    lua_pushcfunction(state, lua_native_component);
    lua_setglobal(state, "__nui_component");
    lua_pushcfunction(state, lua_native_modifier);
    lua_setglobal(state, "__nui_modifier");
    // global_style is also just there in the module env
    push_lua_proxy(state, "global_style");
    lua_setglobal(state, "global_style");
    lua_getglobal(state, "table");
    lua_pushcfunction(state, lua_table_shallow_copy);
    lua_setfield(state, -2, "shallow_copy");
    lua_pop(state, 1);
}

void collect_materialized_components(lua_State *state, const int index,
        std::set<const void *> &visited, NativeUiEvaluation &result) {
    if (!lua_istable(state, index))
        return;
    const int absolute = lua_absindex(state, index);
    const void *identity = lua_topointer(state, absolute);
    if (!visited.insert(identity).second)
        return;
    lua_getfield(state, absolute, "__nui_kind");
    if (const char *kind = lua_tostring(state, -1)) {
        ++result.materialized_component_count;
        append_unique(result.materialized_component_kinds, kind);
    }
    lua_pop(state, 1);
    lua_pushnil(state);
    while (lua_next(state, absolute) != 0) {
        collect_materialized_components(state, -1, visited, result);
        lua_pop(state, 1);
    }
}

std::optional<NativeUiScalar> lua_scalar(lua_State *state, const int index) {
    switch (lua_type(state, index)) {
        case LUA_TBOOLEAN:
            return NativeUiScalar{ lua_toboolean(state, index) != 0 };
        case LUA_TNUMBER:
            return NativeUiScalar{ lua_tonumber(state, index) };
        case LUA_TSTRING: {
            std::size_t size{};
            const char *value = lua_tolstring(state, index, &size);
            return NativeUiScalar{ std::string(value, size) };
        }
        case LUA_TTABLE: {
            const int absolute = lua_absindex(state, index);
            lua_getfield(state, absolute, "__nui_color");
            if (lua_isinteger(state, -1)) {
                const auto color =
                        static_cast<std::uint32_t>(lua_tointeger(state, -1));
                lua_pop(state, 1);
                return NativeUiScalar{ std::string{ "argb:" } +
                                       std::to_string(color) };
            }
            lua_pop(state, 1);
            lua_getfield(state, absolute, "__nui_name");
            if (const char *name = lua_tostring(state, -1)) {
                std::string result{ name };
                lua_pop(state, 1);
                return NativeUiScalar{ std::move(result) };
            }
            lua_pop(state, 1);
            return std::nullopt;
        }
        default:
            return std::nullopt;
    }
}

std::vector<NativeUiProperty> lua_properties(
        lua_State *state, const int index) {
    std::vector<NativeUiProperty> result;
    const int absolute = lua_absindex(state, index);
    lua_pushnil(state);
    while (lua_next(state, absolute) != 0) {
        if (lua_type(state, -2) == LUA_TSTRING) {
            const char *name = lua_tostring(state, -2);
            if (name && !std::string_view(name).starts_with("__"))
                if (auto value = lua_scalar(state, -1))
                    result.push_back({ name, std::move(*value) });
        }
        lua_pop(state, 1);
    }
    std::ranges::sort(result, {}, &NativeUiProperty::name);
    return result;
}

int record_canvas_point(lua_State *state) {
    auto *commands = static_cast<std::vector<NativeUiCanvasCommand> *>(
            lua_touserdata(state, lua_upvalueindex(1)));
    const auto kind = static_cast<NativeUiCanvasCommand::Kind>(
            lua_tointeger(state, lua_upvalueindex(2)));
    if (commands && lua_gettop(state) >= 3) {
        commands->push_back({ kind, lua_tonumber(state, 2) / 1000.0,
                lua_tonumber(state, 3) / 1000.0 });
    }
    return 0;
}

int ignore_canvas_operation(lua_State *) { return 0; }

void extract_canvas_paint(
        lua_State *state, const int component, NativeUiComponentNode &node) {
    lua_getfield(state, component, "paint");
    if (!lua_isfunction(state, -1)) {
        lua_pop(state, 1);
        return;
    }

    // normalized, we map it to the bounds when rendering
    lua_newtable(state);
    const int painter = lua_absindex(state, -1);
    const auto add_method = [&](const char *name,
                                    NativeUiCanvasCommand::Kind kind) {
        lua_pushlightuserdata(state, &node.canvas_commands);
        lua_pushinteger(state, static_cast<lua_Integer>(kind));
        lua_pushcclosure(state, record_canvas_point, 2);
        lua_setfield(state, painter, name);
    };
    add_method("move_to", NativeUiCanvasCommand::Kind::move_to);
    add_method("line_to", NativeUiCanvasCommand::Kind::line_to);
    lua_pushcfunction(state, ignore_canvas_operation);
    lua_setfield(state, painter, "draw_path");
    lua_pushcfunction(state, ignore_canvas_operation);
    lua_setfield(state, painter, "close_path");
    lua_pushvalue(state, painter);
    const auto painter_reference = luaL_ref(state, LUA_REGISTRYINDEX);

    lua_newtable(state);
    lua_pushnumber(state, 1000.0);
    lua_setfield(state, -2, "width");
    lua_pushnumber(state, 1000.0);
    lua_setfield(state, -2, "height");
    if (lua_pcall(state, 2, 0, 0) != LUA_OK) {
        node.canvas_commands.clear();
        lua_pop(state, 1);
        luaL_unref(state, LUA_REGISTRYINDEX, painter_reference);
        return;
    }
    lua_rawgeti(state, LUA_REGISTRYINDEX, painter_reference);
    const int painted = lua_absindex(state, -1);
    for (const char *name : { "stroke_style", "fill_style", "line_width" }) {
        lua_getfield(state, painted, name);
        if (auto value = lua_scalar(state, -1))
            node.canvas_style.push_back({ name, std::move(*value) });
        lua_pop(state, 1);
    }
    lua_pop(state, 1);
    luaL_unref(state, LUA_REGISTRYINDEX, painter_reference);
}

std::optional<NativeUiComponentNode> extract_component(lua_State *state,
        const int index, NativeUiRuntime *runtime, std::string role = {},
        std::string path = "root") {
    if (!lua_istable(state, index))
        return std::nullopt;
    const int absolute = lua_absindex(state, index);
    lua_getfield(state, absolute, "__nui_kind");
    const char *kind = lua_tostring(state, -1);
    if (!kind) {
        lua_pop(state, 1);
        return std::nullopt;
    }
    NativeUiComponentNode node;
    node.kind = kind;
    path += "/" + node.kind;
    node.role = std::move(role);
    lua_pop(state, 1);
    node.properties = lua_properties(state, absolute);
    if (std::string_view(node.kind).ends_with("Canvas") ||
            node.kind == "Canvas")
        extract_canvas_paint(state, absolute, node);

    lua_getfield(state, absolute, "__nui_modifiers");
    if (lua_istable(state, -1)) {
        const int modifiers = lua_absindex(state, -1);
        for (std::size_t i = 1; i <= lua_rawlen(state, modifiers); ++i) {
            lua_rawgeti(state, modifiers, static_cast<lua_Integer>(i));
            if (lua_istable(state, -1)) {
                NativeUiModifier modifier;
                lua_getfield(state, -1, "name");
                if (const char *name = lua_tostring(state, -1))
                    modifier.name = name;
                lua_pop(state, 1);
                lua_getfield(state, -1, "arguments");
                if (lua_istable(state, -1)) {
                    modifier.arguments = lua_properties(state, -1);
                    lua_pushnil(state);
                    while (lua_next(state, -2) != 0) {
                        if (runtime && lua_type(state, -2) == LUA_TSTRING &&
                                lua_isfunction(state, -1)) {
                            const char *phase = lua_tostring(state, -2);
                            lua_pushvalue(state, -1);
                            const auto key = path +
                                             "/modifier:" + std::to_string(i) +
                                             ":" + modifier.name + ":" +
                                             (phase ? phase : "");
                            auto [slot, inserted] =
                                    runtime->action_slots.try_emplace(key, 0);
                            int reference;
                            if (inserted) {
                                reference = luaL_ref(state, LUA_REGISTRYINDEX);
                                slot->second =
                                        (runtime->next_action++ << 32U) |
                                        static_cast<std::uint32_t>(reference);
                            } else {
                                reference = static_cast<int>(
                                        static_cast<std::uint32_t>(
                                                slot->second));
                                lua_rawseti(
                                        state, LUA_REGISTRYINDEX, reference);
                            }
                            runtime->materialized_actions.insert(key);
                            runtime->active_actions.insert(slot->second);
                            modifier.actions.push_back(
                                    { phase ? phase : "", slot->second });
                            if (std::getenv("EKPHORE_PROFILE_CIRCULAR_DRAG") &&
                                    runtime->next_action > 85 &&
                                    runtime->next_action < 120)
                                std::fprintf(stderr,
                                        "native-action-register %s %llu ref=%d\n",
                                        phase ? phase : "",
                                        static_cast<unsigned long long>(
                                                modifier.actions.back().id),
                                        reference);
                        }
                        if (lua_type(state, -2) == LUA_TSTRING &&
                                lua_istable(state, -1)) {
                            const char *argument_name = lua_tostring(state, -2);
                            if (argument_name)
                                if (auto child = extract_component(state, -1,
                                            runtime,
                                            "modifier:" + modifier.name + ":" +
                                                    std::to_string(i) + ":" +
                                                    argument_name,
                                            path + "/modifier:" +
                                                    std::to_string(i) + ":" +
                                                    argument_name))
                                    node.children.push_back(std::move(*child));
                        }
                        lua_pop(state, 1);
                    }
                    for (std::size_t argument = 1;
                            argument <= lua_rawlen(state, -1); ++argument) {
                        lua_rawgeti(
                                state, -1, static_cast<lua_Integer>(argument));
                        if (auto value = lua_scalar(state, -1))
                            modifier.arguments.push_back(
                                    { "#" + std::to_string(argument),
                                            std::move(*value) });
                        else if (auto child = extract_component(state, -1,
                                         runtime,
                                         "modifier:" + modifier.name + ":" +
                                                 std::to_string(i) + ":#" +
                                                 std::to_string(argument),
                                         path + "/modifier:" +
                                                 std::to_string(i) + ":#" +
                                                 std::to_string(argument)))
                            node.children.push_back(std::move(*child));
                        lua_pop(state, 1);
                    }
                }
                lua_pop(state, 1);
                node.modifiers.push_back(std::move(modifier));
            }
            lua_pop(state, 1);
        }
    }
    lua_pop(state, 1);

    const auto positional = lua_rawlen(state, absolute);
    for (std::size_t i = 1; i <= positional; ++i) {
        lua_rawgeti(state, absolute, static_cast<lua_Integer>(i));
        if (auto child = extract_component(state, -1, runtime, {},
                    path + "/child:" + std::to_string(i))) {
            // ForEach doesn't get its own node, its children go straight in
            // the stack
            const std::string_view child_kind = child->kind;
            const bool transparent_for_each =
                    (child_kind == "ForEach" ||
                            child_kind.ends_with(".ForEach")) &&
                    child->role.empty() && child->modifiers.empty();
            if (transparent_for_each) {
                node.children.insert(node.children.end(),
                        std::make_move_iterator(child->children.begin()),
                        std::make_move_iterator(child->children.end()));
            } else {
                node.children.push_back(std::move(*child));
            }
        }
        lua_pop(state, 1);
    }
    lua_pushnil(state);
    while (lua_next(state, absolute) != 0) {
        if (lua_type(state, -2) == LUA_TSTRING && lua_istable(state, -1)) {
            const char *name = lua_tostring(state, -2);
            if (name && !std::string_view(name).starts_with("__"))
                if (auto child = extract_component(state, -1, runtime, name,
                            path + "/property:" + name))
                    node.children.push_back(std::move(*child));
        }
        lua_pop(state, 1);
    }
    return node;
}

void preload_external_modules(lua_State *state,
        const NativeUiModuleGraph &graph,
        const LuaParameterContext *parameters) {
    lua_getglobal(state, "package");
    lua_getfield(state, -1, "preload");
    // some products do pcall(require, "kontakt") which the scanner can't see,
    // so always install it
    lua_pushlightuserdata(state, const_cast<LuaParameterContext *>(parameters));
    lua_pushcclosure(state, lua_connection_loader, 1);
    lua_setfield(state, -2, "kontakt");
    for (const auto &module : graph.external_modules) {
        if (module == "use_connection" || module == "kontakt") {
            lua_pushlightuserdata(
                    state, const_cast<LuaParameterContext *>(parameters));
            lua_pushcclosure(state, lua_connection_loader, 1);
        } else {
            lua_pushcfunction(state,
                    module == "native_ui"        ? lua_native_ui_loader
                    : module == "native_ui_util" ? lua_native_ui_util_loader
                                                 : lua_proxy_loader);
        }
        lua_setfield(state, -2, module.c_str());
    }
    lua_pop(state, 2);
}

} // namespace

std::vector<std::string> native_ui_imports(const std::string_view source) {
    std::vector<std::string> imports;
    std::size_t cursor{};
    while (cursor < source.size()) {
        skip_space_and_comments(source, cursor);
        if (cursor >= source.size())
            break;
        if (source[cursor] == '\'' || source[cursor] == '"') {
            (void)quoted_string(source, cursor);
            continue;
        }
        if (source[cursor] == '[' && skip_long_bracket(source, cursor))
            continue;
        if (!identifier_start(source[cursor])) {
            ++cursor;
            continue;
        }
        const auto begin = cursor++;
        while (cursor < source.size() && identifier_continue(source[cursor]))
            ++cursor;
        if (source.substr(begin, cursor - begin) != "require")
            continue;
        skip_space_and_comments(source, cursor);
        bool parenthesized{};
        if (cursor < source.size() && source[cursor] == '(') {
            parenthesized = true;
            ++cursor;
            skip_space_and_comments(source, cursor);
        }
        auto module = quoted_string(source, cursor);
        if (!module || module->empty())
            continue;
        if (parenthesized) {
            skip_space_and_comments(source, cursor);
            if (cursor < source.size() && source[cursor] == ')')
                ++cursor;
        }
        if (std::ranges::find(imports, *module) == imports.end())
            imports.push_back(std::move(*module));
    }
    return imports;
}

std::vector<std::string> native_ui_declarative_components(
        const std::string_view source) {
    std::vector<std::string> components;
    std::size_t cursor{};
    while (cursor < source.size()) {
        skip_space_and_comments(source, cursor);
        if (cursor >= source.size())
            break;
        if (source[cursor] == '\'' || source[cursor] == '"') {
            (void)quoted_string(source, cursor);
            continue;
        }
        if (source[cursor] == '[' && skip_long_bracket(source, cursor))
            continue;
        if (source[cursor++] != '@')
            continue;
        skip_space_and_comments(source, cursor);
        if (cursor < source.size() && source[cursor] == '{')
            continue;
        if (auto component = identifier_path(source, cursor))
            append_unique(components, std::move(*component));
    }
    return components;
}

std::vector<std::string> native_ui_method_calls(const std::string_view source) {
    std::vector<std::string> methods;
    std::size_t cursor{};
    while (cursor < source.size()) {
        skip_space_and_comments(source, cursor);
        if (cursor >= source.size())
            break;
        if (source[cursor] == '\'' || source[cursor] == '"') {
            (void)quoted_string(source, cursor);
            continue;
        }
        if (source[cursor] == '[' && skip_long_bracket(source, cursor))
            continue;
        if (source[cursor++] != '.')
            continue;
        const auto method = identifier_path(source, cursor);
        if (!method)
            continue;
        auto after = cursor;
        skip_space_and_comments(source, after);
        if (after < source.size() && source[after] == '(')
            append_unique(methods, *method);
    }
    return methods;
}

NativeUiLowering lower_native_ui_declarative(const std::string_view source) {
    return DeclarativeLowerer(source).run();
}

NativeUiModuleGraph resolve_native_ui_modules(
        const std::string_view root_module,
        const NativeUiModuleSource &source) {
    if (root_module.empty())
        return {};
    return Resolver(source).run(root_module);
}

NativeUiEvaluation evaluate_native_ui_lua(const std::string_view root_module,
        const NativeUiModuleGraph &graph,
        const NativeUiParameterSource &parameters,
        const NativeUiControlPropertySource &control_properties,
        const NativeUiPendingParameterSource &pending) {
    NativeUiEvaluation result;
    auto runtime = std::make_shared<NativeUiRuntime>();
    runtime->source = parameters;
    runtime->control_properties = control_properties;
    runtime->pending_source = pending;
    lua_State *state = runtime->state;
    if (!state) {
        result.diagnostics.emplace_back("could not create native UI Lua state");
        return result;
    }
    luaL_openlibs(state);
    install_declarative_runtime(state);
    // Circular 1.5.1 uses this without declaring it, treat it as off
    if (std::ranges::any_of(graph.modules, [](const auto &module) {
            return module.name == "pages/sequencer_page" &&
                   module.source.find("lane_hovered = lane_hovered()") !=
                           std::string::npos;
        })) {
        lua_pushcfunction(state, [](lua_State *callback) {
            lua_pushboolean(callback, false);
            return 1;
        });
        lua_setglobal(state, "lane_hovered");
    }
    lua_pushlightuserdata(state, runtime.get());
    lua_setfield(state, LUA_REGISTRYINDEX, "ekphore.native_ui.runtime");
    auto *parameter_context = static_cast<LuaParameterContext *>(
            lua_newuserdatauv(state, sizeof(LuaParameterContext), 0));
    *parameter_context = LuaParameterContext{ runtime.get() };
    runtime->context_reference = luaL_ref(state, LUA_REGISTRYINDEX);
    preload_external_modules(state, graph, parameter_context);
    lua_getglobal(state, "package");
    lua_getfield(state, -1, "preload");
    for (const auto &module : graph.modules) {
        const auto chunk_name = "@" + module.name + ".nui";
        if (luaL_loadbufferx(state, module.source.data(), module.source.size(),
                    chunk_name.c_str(), "t") != LUA_OK) {
            const char *message = lua_tostring(state, -1);
            result.diagnostics.push_back("native UI compile " + module.name +
                                         ": " +
                                         (message ? message : "unknown error"));
            lua_pop(state, 1);
            continue;
        }
        lua_setfield(state, -2, module.name.c_str());
    }
    lua_pop(state, 2);

    lua_getglobal(state, "require");
    const auto canonical = canonical_module(root_module);
    lua_pushlstring(state, canonical.data(), canonical.size());
    if (lua_pcall(state, 1, 1, 0) != LUA_OK) {
        const char *message = lua_tostring(state, -1);
        result.diagnostics.push_back("native UI evaluate " + canonical + ": " +
                                     (message ? message : "unknown error"));
        return result;
    }
    result.loaded = true;
    result.root_value_type = luaL_typename(state, -1);
    if (lua_isfunction(state, -1)) {
        struct InitialMaterializing {
            bool &flag;
            ~InitialMaterializing() { flag = false; }
        } materializing{ runtime->materializing };
        runtime->materializing = true;
        lua_pushvalue(state, -1);
        runtime->root_reference = luaL_ref(state, LUA_REGISTRYINDEX);
        runtime->state_owner = "root";
        runtime->instance_occurrences.clear();
        lua_pushvalue(state, -1);
        if (lua_pcall(state, 0, 1, 0) != LUA_OK) {
            const char *message = lua_tostring(state, -1);
            result.diagnostics.push_back("native UI materialize " + canonical +
                                         ": " +
                                         (message ? message : "unknown error"));
            return result;
        }
        result.materialized = true;
        std::string factory_diagnostic;
        if (!push_materialized(state, -1, factory_diagnostic)) {
            result.materialized = false;
            result.diagnostics.push_back("native UI expand " + canonical +
                                         ": " + factory_diagnostic);
            return result;
        }
        lua_remove(state, -2);
        result.materialized_value_type = luaL_typename(state, -1);
        std::set<const void *> visited;
        collect_materialized_components(state, -1, visited, result);
        if (auto root = extract_component(state, -1, runtime.get())) {
            const auto attach_control_values =
                    [&](const auto &self, NativeUiComponentNode &node) -> void {
                const auto attach = [&](const std::string_view id_property,
                                            const std::string_view
                                                    value_property) {
                    const auto control = std::ranges::find(node.properties,
                            id_property, &NativeUiProperty::name);
                    if (control == node.properties.end())
                        return;
                    const auto *name =
                            std::get_if<std::string>(&control->value);
                    if (!name)
                        return;
                    const auto value = parameters(*name);
                    if (!value)
                        return;
                    std::visit(
                            [&](const auto &item) {
                                using Value = std::decay_t<decltype(item)>;
                                if constexpr (std::is_same_v<Value, bool> ||
                                              std::is_same_v<Value, double> ||
                                              std::is_same_v<Value,
                                                      std::string>)
                                    node.properties.push_back(
                                            { std::string{ value_property },
                                                    item });
                                else if constexpr (std::is_same_v<Value,
                                                           std::int64_t>)
                                    node.properties.push_back(
                                            { std::string{ value_property },
                                                    static_cast<double>(
                                                            item) });
                            },
                            *value);
                };
                attach("control_id", "__control_value");
                attach("knob_control_id", "__knob_control_value");
                attach("power_control_id", "__power_control_value");
                for (auto &child : node.children)
                    self(self, child);
            };
            attach_control_values(attach_control_values, *root);
            result.component_roots.push_back(std::move(*root));
        }
    }
    result.runtime = std::move(runtime);
    return result;
}

void update_native_ui_sources(const std::shared_ptr<NativeUiRuntime> &runtime,
        NativeUiParameterSource parameters,
        NativeUiControlPropertySource properties,
        NativeUiPendingParameterSource pending) {
    if (!runtime)
        return;
    std::scoped_lock lock(runtime->mutex);
    runtime->source = std::move(parameters);
    runtime->control_properties = std::move(properties);
    runtime->pending_source = std::move(pending);
    ++runtime->computed_epoch;
}

NativeUiEvaluation rematerialize_native_ui(
        const std::shared_ptr<NativeUiRuntime> &runtime) {
    const auto profile_start = std::chrono::steady_clock::now();
    profile_factory_us = profile_compact_us = profile_foreach_us = 0;
    profile_factory_count = profile_compact_count = 0;
    NativeUiEvaluation result;
    result.runtime = runtime;
    if (!runtime)
        return result;
    std::scoped_lock lock(runtime->mutex);
    if (!runtime->state || runtime->root_reference == LUA_NOREF)
        return result;
    struct Materializing {
        bool &flag;
        bool previous;
        ~Materializing() { flag = previous; }
    } materializing{ runtime->materializing, runtime->materializing };
    runtime->materializing = true;
    lua_State *state = runtime->state;
    runtime->state_owner = "root";
    runtime->instance_occurrences.clear();
    lua_settop(state, 0);
    lua_rawgeti(state, LUA_REGISTRYINDEX, runtime->root_reference);
    if (!lua_isfunction(state, -1)) {
        lua_settop(state, 0);
        return result;
    }
    result.loaded = true;
    result.root_value_type = "function";
    if (lua_pcall(state, 0, 1, 0) != LUA_OK) {
        const char *message = lua_tostring(state, -1);
        result.diagnostics.push_back(
                std::string{ "native UI rematerialize: " } +
                (message ? message : "unknown error"));
        lua_settop(state, 0);
        return result;
    }
    const auto profile_root = std::chrono::steady_clock::now();
    result.materialized = true;
    std::string factory_diagnostic;
    if (!push_materialized(state, -1, factory_diagnostic)) {
        result.materialized = false;
        result.diagnostics.push_back("native UI expand: " + factory_diagnostic);
        lua_settop(state, 0);
        return result;
    }
    lua_remove(state, -2);
    const auto profile_push = std::chrono::steady_clock::now();
    result.materialized_value_type = luaL_typename(state, -1);
    std::set<const void *> visited;
    collect_materialized_components(state, -1, visited, result);
    const auto profile_collect = std::chrono::steady_clock::now();
    runtime->materialized_actions.clear();
    if (auto root = extract_component(state, -1, runtime.get())) {
        const auto attach_control_values =
                [&](const auto &self, NativeUiComponentNode &node) -> void {
            const auto attach = [&](const std::string_view id_property,
                                        const std::string_view value_property) {
                const auto control = std::ranges::find(
                        node.properties, id_property, &NativeUiProperty::name);
                if (control == node.properties.end())
                    return;
                const auto *name = std::get_if<std::string>(&control->value);
                if (!name || !runtime->source)
                    return;
                const auto value = runtime->source(*name);
                if (!value)
                    return;
                std::visit(
                        [&](const auto &item) {
                            using Value = std::decay_t<decltype(item)>;
                            if constexpr (std::is_same_v<Value, bool> ||
                                          std::is_same_v<Value, double> ||
                                          std::is_same_v<Value, std::string>)
                                node.properties.push_back(
                                        { std::string{ value_property },
                                                item });
                            else if constexpr (std::is_same_v<Value,
                                                       std::int64_t>)
                                node.properties.push_back(
                                        { std::string{ value_property },
                                                static_cast<double>(item) });
                        },
                        *value);
            };
            attach("control_id", "__control_value");
            attach("knob_control_id", "__knob_control_value");
            attach("power_control_id", "__power_control_value");
            for (auto &child : node.children)
                self(self, child);
        };
        attach_control_values(attach_control_values, *root);
        result.component_roots.push_back(std::move(*root));
    }
    for (auto slot = runtime->action_slots.begin();
            slot != runtime->action_slots.end();) {
        if (runtime->materialized_actions.contains(slot->first)) {
            ++slot;
            continue;
        }
        runtime->active_actions.erase(slot->second);
        luaL_unref(state, LUA_REGISTRYINDEX,
                static_cast<int>(static_cast<std::uint32_t>(slot->second)));
        slot = runtime->action_slots.erase(slot);
    }
    lua_settop(state, 0);
    if (std::getenv("EKPHORE_PROFILE_CIRCULAR_DRAG")) {
        const auto micros = [](const auto a, const auto b) {
            return std::chrono::duration_cast<std::chrono::microseconds>(b - a)
                    .count();
        };
        std::fprintf(stderr,
                "native-remat-us root=%lld push=%lld collect=%lld extract=%lld "
                "factory=%lld/%lld compact=%lld/%lld foreach=%lld\n",
                static_cast<long long>(micros(profile_start, profile_root)),
                static_cast<long long>(micros(profile_root, profile_push)),
                static_cast<long long>(micros(profile_push, profile_collect)),
                static_cast<long long>(micros(
                        profile_collect, std::chrono::steady_clock::now())),
                static_cast<long long>(profile_factory_us),
                static_cast<long long>(profile_factory_count),
                static_cast<long long>(profile_compact_us),
                static_cast<long long>(profile_compact_count),
                static_cast<long long>(profile_foreach_us));
    }
    return result;
}

std::vector<NativeUiParameterWrite> dispatch_native_ui_action(
        const std::shared_ptr<NativeUiRuntime> &runtime,
        const std::uint64_t action, const NativeUiGestureEvent &event) {
    if (!runtime)
        return {};
    std::scoped_lock lock(runtime->mutex);
    if (!runtime->state || action == 0 ||
            !runtime->active_actions.contains(action)) {
        detail::gesture_trace("action-rejected id=%llu",
                static_cast<unsigned long long>(action));
        return {};
    }
    detail::gesture_trace(
            "action id=%llu pointer=%g,%g delta=%g,%g frame=%g,%g,%g,%g",
            static_cast<unsigned long long>(action), event.x, event.y,
            event.translation_x, event.translation_y, event.rectangle_x,
            event.rectangle_y, event.rectangle_width, event.rectangle_height);
    lua_State *state = runtime->state;
    runtime->writes.clear();
    const auto reference = static_cast<int>(static_cast<std::uint32_t>(action));
    lua_rawgeti(state, LUA_REGISTRYINDEX, reference);
    if (!lua_isfunction(state, -1)) {
        lua_pop(state, 1);
        return {};
    }
    lua_newtable(state);
    const auto number = [&](const char *name, const double value) {
        lua_pushnumber(state, value);
        lua_setfield(state, -2, name);
    };
    number("x", event.x);
    number("y", event.y);
    number("start_x", event.start_x);
    number("start_y", event.start_y);
    number("translation_x", event.translation_x);
    number("translation_y", event.translation_y);
    number("rectangle_width", event.rectangle_width);
    number("rectangle_height", event.rectangle_height);
    number("modifier_flags", event.modifiers);
    // old lowered callbacks still use these
    const auto point = [&](const char *name, const double x, const double y) {
        lua_newtable(state);
        lua_pushnumber(state, x);
        lua_setfield(state, -2, "x");
        lua_pushnumber(state, y);
        lua_setfield(state, -2, "y");
        lua_setfield(state, -2, name);
    };
    point("location", event.x, event.y);
    point("start_location", event.start_x, event.start_y);
    // DraggableDual picks the nearest point with this, without it every drag
    // moves the first handle and folds the curve
    point("position", event.x - event.rectangle_x, event.y - event.rectangle_y);
    point("delta", event.translation_x, event.translation_y);
    point("translation", event.translation_x, event.translation_y);
    lua_newtable(state);
    lua_pushnumber(state, event.rectangle_x);
    lua_setfield(state, -2, "x");
    lua_pushnumber(state, event.rectangle_y);
    lua_setfield(state, -2, "y");
    lua_pushnumber(state, event.rectangle_width);
    lua_setfield(state, -2, "width");
    lua_pushnumber(state, event.rectangle_height);
    lua_setfield(state, -2, "height");
    lua_pushvalue(state, -1);
    lua_setfield(state, -3, "frame");
    lua_setfield(state, -2, "rectangle");
    lua_newtable(state);
    lua_pushboolean(state, (event.modifiers & 1U) != 0U);
    lua_setfield(state, -2, "shift");
    lua_pushboolean(state, (event.modifiers & 2U) != 0U);
    lua_setfield(state, -2, "control");
    lua_pushboolean(state, (event.modifiers & 4U) != 0U);
    lua_setfield(state, -2, "alt");
    lua_pushboolean(state, (event.modifiers & 8U) != 0U);
    lua_setfield(state, -2, "command");
    lua_setfield(state, -2, "modifiers");
    lua_pushcfunction(state, [](lua_State *error_state) {
        const auto *message = lua_tostring(error_state, 1);
        luaL_traceback(error_state, error_state,
                message ? message : "native action error", 1);
        return 1;
    });
    lua_insert(state, -3);
    const int error_handler = lua_absindex(state, -3);
    if (lua_pcall(state, 1, 0, error_handler) != LUA_OK) {
        detail::gesture_trace("action-error id=%llu message=%s",
                static_cast<unsigned long long>(action),
                lua_tostring(state, -1));
        if (std::getenv("EKPHORE_PROFILE_CIRCULAR_DRAG"))
            std::fprintf(stderr, "native-ui-action-error: %s\n",
                    lua_tostring(state, -1));
        lua_pop(state, 1);
    }
    lua_remove(state, error_handler);
    return std::exchange(runtime->writes, {});
}

NativeUiEvaluation rematerialize_native_ui_with_sources(
        const std::shared_ptr<NativeUiRuntime> &runtime,
        NativeUiParameterSource parameters,
        NativeUiControlPropertySource properties,
        NativeUiPendingParameterSource pending) {
    if (!runtime)
        return {};
    std::scoped_lock lock(runtime->mutex);
    update_native_ui_sources(runtime, std::move(parameters),
            std::move(properties), std::move(pending));
    return rematerialize_native_ui(runtime);
}

std::vector<NativeUiParameterWrite> dispatch_native_ui_action_with_sources(
        const std::shared_ptr<NativeUiRuntime> &runtime,
        const std::uint64_t action, const NativeUiGestureEvent &event,
        NativeUiParameterSource parameters,
        NativeUiControlPropertySource properties,
        NativeUiPendingParameterSource pending) {
    if (!runtime)
        return {};
    std::scoped_lock lock(runtime->mutex);
    update_native_ui_sources(runtime, std::move(parameters),
            std::move(properties), std::move(pending));
    return dispatch_native_ui_action(runtime, action, event);
}

std::vector<std::string> validate_native_ui_lua(
        const NativeUiModuleGraph &graph) {
    std::vector<std::string> diagnostics;
    struct LuaState {
        lua_State *value{ luaL_newstate() };
        ~LuaState() {
            if (value)
                lua_close(value);
        }
    } state;
    if (!state.value) {
        diagnostics.emplace_back("could not create native UI Lua state");
        return diagnostics;
    }
    for (const auto &module : graph.modules) {
        const auto chunk_name = "@" + module.name + ".nui";
        if (luaL_loadbufferx(state.value, module.source.data(),
                    module.source.size(), chunk_name.c_str(), "t") == LUA_OK) {
            lua_pop(state.value, 1);
            continue;
        }
        const char *message = lua_tostring(state.value, -1);
        diagnostics.push_back("native UI compile " + module.name + ": " +
                              (message ? message : "unknown error"));
        lua_pop(state.value, 1);
    }
    return diagnostics;
}

} // namespace ekphore
