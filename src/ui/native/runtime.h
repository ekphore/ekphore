// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <ekphore/native_ui.h>
#include <map>
#include <mutex>
#include <set>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

namespace ekphore {

struct NativeUiRuntime {
    // lock for anything Lua, NEVER from audio
    std::recursive_mutex mutex;
    lua_State *state{ luaL_newstate() };
    NativeUiParameterSource source;
    NativeUiControlPropertySource control_properties;
    NativeUiPendingParameterSource pending_source;
    std::vector<NativeUiParameterWrite> writes;
    std::uint64_t next_action{ 1 };
    // replace in place, otherwise a captured gesture keeps the old props
    std::map<std::string, std::uint64_t> action_slots;
    std::set<std::string> materialized_actions;
    std::set<std::uint64_t> active_actions;
    int context_reference{ LUA_NOREF };
    int root_reference{ LUA_NOREF };
    std::map<std::string, int> state_references;
    std::vector<int> context_keys;
    std::string state_owner{ "modules" };
    std::map<std::string, std::size_t> instance_occurrences;
    std::uint64_t computed_epoch{ 1 };
    bool materializing{};

    ~NativeUiRuntime() {
        if (state)
            lua_close(state);
    }
};

} // namespace ekphore
