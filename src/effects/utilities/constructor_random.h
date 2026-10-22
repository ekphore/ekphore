// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "../control/support.h"
#include <algorithm>
#include <cstddef>
#include <libnki/libnki.h>
#include <mutex>
#include <optional>
#include <random>

namespace ekphore::effect_runtime {

enum class ConstructorScope { group, bus, insert, send, main };

struct ConstructorRandomOwner {
    // load only, live replacements use the atomic stream
    std::mutex load_mutex;
    dsp::AtomicEffektrigRandom01 stream{ std::random_device{}() };
};

// the first call grabs entropy, so NEVER from audio. reloads keep the stream
inline ConstructorRandomOwner &constructor_random_owner() {
    static ConstructorRandomOwner owner;
    return owner;
}

template <class Next>
std::optional<float> retained_constructor_phase(
        libnki::GroupEffectType type, bool paired, Next &&next) {
    if (type != libnki::GroupEffectType::flair &&
            type != libnki::GroupEffectType::phasis)
        return std::nullopt;
    float phase = next();
    if (paired)
        phase = next();
    return phase;
}

// only for effects we actually build, NOT when copying one into a voice
template <class Next, class Publish>
void publish_constructor_phases(const libnki::InstrumentInfo &instrument,
        Next &&next, Publish &&publish) {
    const auto chain = [&](const auto &effects, ConstructorScope scope,
                               std::size_t program, std::size_t owner,
                               bool paired) {
        for (int slot = 0; slot < 8; ++slot) {
            const auto found = std::find_if(
                    effects.begin(), effects.end(), [&](const auto &effect) {
                        return effect.slot_index == slot;
                    });
            if (found == effects.end() || !found->type)
                continue;
            // bypassed ones too, replacements keep their phase
            if (const auto phase = retained_constructor_phase(
                        *found->type, paired, next))
                publish(scope, program, owner, slot, *phase);
        }
    };
    for (std::size_t p = 0; p < instrument.programs.size(); ++p) {
        for (std::size_t g = 0; g < instrument.groups.size(); ++g)
            if (instrument.groups[g].program_index.value_or(0) == p)
                chain(instrument.groups[g].effects, ConstructorScope::group, p,
                        g, false);
        const auto &program = instrument.programs[p];
        for (std::size_t b = 0;
                b < std::min<std::size_t>(16, program.insert_buses.size()); ++b)
            chain(program.insert_buses[b].effects, ConstructorScope::bus, p, b,
                    true);
        chain(program.insert_effects, ConstructorScope::insert, p, 0, true);
        chain(program.send_effects, ConstructorScope::send, p, 0, true);
        chain(program.main_effects, ConstructorScope::main, p, 0, true);
    }
}

} // namespace ekphore::effect_runtime
